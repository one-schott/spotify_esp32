#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <base64.h>
#include "secrets.h"

// WiFi credentials
const char* ssid = WIFI_SSID;
const char* password = WIFI_PASSWORD;

// Spotify API credentials
const char* spotify_client_id = SPOTIFY_CLIENT_ID;
const char* spotify_client_secret = SPOTIFY_CLIENT_SECRET;
const char* spotify_refresh_token = SPOTIFY_REFRESH_TOKEN;

// Global variables
String access_token = "";
String current_song = "Not playing";
String current_artist = "Unknown";
unsigned long last_token_refresh = 0;
unsigned long last_song_check = 0;
const unsigned long token_refresh_interval = 3000000; // 50 minutes
const unsigned long song_check_interval = 5000; // 5 seconds

WebServer server(80);
WiFiClientSecure client;

// Function declarations
void connectWiFi();
bool refreshAccessToken();
bool getCurrentlyPlaying();
void handleRoot();
void handleRefresh();

void setup() {
  Serial.begin(115200);
  pinMode(LED_BUILTIN, OUTPUT);
  
  Serial.println("\n\nSpotify Now Playing - ESP32");
  Serial.println("============================");
  
  connectWiFi();
  
  client.setInsecure(); // Skip SSL certificate verification
  
  // Get initial access token
  if (refreshAccessToken()) {
    Serial.println("Access token obtained successfully!");
    getCurrentlyPlaying();
  } else {
    Serial.println("Failed to get access token");
  }
  
  // Setup web server routes
  server.on("/", handleRoot);
  server.on("/refresh", handleRefresh);
  server.onNotFound([]() {
    server.send(404, "text/plain", "Not Found");
  });
  server.begin();
  
  Serial.println("Web server started!");
  Serial.print("Visit: http://");
  Serial.println(WiFi.localIP());
}

void loop() {
  server.handleClient();
  
  unsigned long current_time = millis();
  
  // Refresh access token if needed
  if (current_time - last_token_refresh > token_refresh_interval) {
    Serial.println("Refreshing access token...");
    refreshAccessToken();
  }
  
  // Check currently playing song
  if (current_time - last_song_check > song_check_interval) {
    getCurrentlyPlaying();
  }
  
  // Blink LED to show it's alive
  digitalWrite(LED_BUILTIN, millis() % 2000 < 100);
}

void connectWiFi() {
  Serial.print("Connecting to WiFi: ");
  Serial.println(ssid);
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 60) { // Increased to 30 seconds
    delay(500);
    Serial.print(".");
    attempts++;
    
    if (attempts % 20 == 0) {
      Serial.println();
      Serial.print("Still trying... Status: ");
      Serial.println(WiFi.status());
    }
  }
  
  Serial.println();
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi connected!");
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    Serial.print("Signal strength (RSSI): ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");
  } else {
    Serial.println("WiFi connection failed!");
    Serial.print("WiFi status code: ");
    Serial.println(WiFi.status());
    Serial.println("Check: 1) Correct password 2) 2.4GHz network 3) Signal strength");
  }
}

bool refreshAccessToken() {
  Serial.println("Requesting new access token...");
  
  if (!client.connect("accounts.spotify.com", 443)) {
    Serial.println("Connection to Spotify failed");
    return false;
  }
  
  // Create authorization header
  String auth = String(spotify_client_id) + ":" + String(spotify_client_secret);
  String auth_encoded = base64::encode(auth);
  
  // Create POST body
  String body = "grant_type=refresh_token&refresh_token=" + String(spotify_refresh_token);
  
  // Send HTTP POST request
  client.println("POST /api/token HTTP/1.1");
  client.println("Host: accounts.spotify.com");
  client.println("Authorization: Basic " + auth_encoded);
  client.println("Content-Type: application/x-www-form-urlencoded");
  client.print("Content-Length: ");
  client.println(body.length());
  client.println("Connection: close");
  client.println();
  client.println(body);
  
  // Wait for response
  while (client.connected() && !client.available()) {
    delay(10);
  }
  
  // Skip HTTP headers
  bool headers_end = false;
  while (client.available() && !headers_end) {
    String line = client.readStringUntil('\n');
    if (line == "\r") {
      headers_end = true;
    }
  }
  
  // Read JSON response
  String response = client.readString();
  client.stop();
  
  // Debug output
  Serial.println("Token Response:");
  Serial.println(response);
  Serial.println("---");
  
  // Parse JSON
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, response);
  
  if (error) {
    Serial.print("JSON parsing failed: ");
    Serial.println(error.c_str());
    return false;
  }
  
  if (doc["access_token"].is<const char*>()) {
    access_token = doc["access_token"].as<String>();
    last_token_refresh = millis();
    Serial.println("Access token refreshed successfully!");
    Serial.print("Token length: ");
    Serial.println(access_token.length());
    return true;
  } else {
    Serial.println("No access token in response");
    Serial.println("Available keys in response:");
    for (JsonPair kv : doc.as<JsonObject>()) {
      Serial.println(kv.key().c_str());
    }
    return false;
  }
}

bool getCurrentlyPlaying() {
  static unsigned long last_error_print = 0;
  
  if (access_token.isEmpty()) {
    if (millis() - last_error_print > 10000) { // Only print every 10 seconds
      Serial.println("No access token available");
      last_error_print = millis();
    }
    return false;
  }
  
  if (!client.connect("api.spotify.com", 443)) {
    Serial.println("Connection to Spotify API failed");
    return false;
  }
  
  // Send HTTP GET request
  client.println("GET /v1/me/player/currently-playing HTTP/1.1");
  client.println("Host: api.spotify.com");
  client.println("Authorization: Bearer " + access_token);
  client.println("Connection: close");
  client.println();
  
  // Wait for response
  while (client.connected() && !client.available()) {
    delay(10);
  }
  
  // Read status code
  String status_line = client.readStringUntil('\n');
  int status_code = 0;
  if (status_line.indexOf("HTTP/1.1") >= 0) {
    status_code = status_line.substring(9, 12).toInt();
  }
  
  // Skip remaining headers
  bool headers_end = false;
  while (client.available() && !headers_end) {
    String line = client.readStringUntil('\n');
    if (line == "\r") {
      headers_end = true;
    }
  }
  
  // Handle 204 No Content (nothing playing)
  if (status_code == 204) {
    current_song = "Nothing playing";
    current_artist = "";
    Serial.println("Nothing currently playing");
    client.stop();
    last_song_check = millis();
    return true;
  }
  
  // Read JSON response
  String response = client.readString();
  client.stop();
  
  if (response.isEmpty()) {
    Serial.println("Empty response from Spotify");
    last_song_check = millis();
    return false;
  }
  
  // Parse JSON
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, response);
  
  if (error) {
    Serial.print("JSON parsing failed: ");
    Serial.println(error.c_str());
    last_song_check = millis();
    return false;
  }
  
  // Extract song and artist info
  if (doc["item"].is<JsonObject>()) {
    String new_song = doc["item"]["name"].as<String>();
    String new_artist = doc["item"]["artists"][0]["name"].as<String>();
    
    // Only update and print if song changed
    if (new_song != current_song || new_artist != current_artist) {
      current_song = new_song;
      current_artist = new_artist;
      Serial.println("\n♪ Now Playing:");
      Serial.println("   Song: " + current_song);
      Serial.println("   Artist: " + current_artist);
    }
  } else {
    current_song = "Nothing playing";
    current_artist = "";
  }
  
  last_song_check = millis();
  return true;
}

void handleRoot() {
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<title>Spotify Now Playing</title>";
  html += "<style>";
  html += "body { font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; ";
  html += "background: linear-gradient(135deg, #1DB954 0%, #191414 100%); ";
  html += "color: white; margin: 0; padding: 20px; min-height: 100vh; ";
  html += "display: flex; flex-direction: column; align-items: center; justify-content: center; }";
  html += ".container { background: rgba(0,0,0,0.7); padding: 40px; ";
  html += "border-radius: 20px; max-width: 500px; width: 100%; box-shadow: 0 8px 32px rgba(0,0,0,0.3); }";
  html += "h1 { text-align: center; margin: 0 0 30px 0; font-size: 2em; }";
  html += ".music-icon { text-align: center; font-size: 4em; margin-bottom: 20px; }";
  html += ".info { margin: 20px 0; }";
  html += ".label { color: #1DB954; font-weight: bold; font-size: 0.9em; margin-bottom: 5px; }";
  html += ".value { font-size: 1.5em; word-wrap: break-word; }";
  html += ".button { background: #1DB954; color: white; border: none; ";
  html += "padding: 15px 30px; font-size: 1em; border-radius: 30px; ";
  html += "cursor: pointer; width: 100%; margin-top: 20px; font-weight: bold; }";
  html += ".button:hover { background: #1ed760; }";
  html += ".status { text-align: center; color: #b3b3b3; font-size: 0.9em; margin-top: 20px; }";
  html += "</style>";
  html += "<script>";
  html += "function refresh() { fetch('/refresh').then(() => location.reload()); }";
  html += "setInterval(() => location.reload(), 10000);"; // Auto-refresh every 10 seconds
  html += "</script>";
  html += "</head><body>";
  html += "<div class='container'>";
  html += "<div class='music-icon'>🎵</div>";
  html += "<h1>Now Playing</h1>";
  html += "<div class='info'>";
  html += "<div class='label'>SONG</div>";
  html += "<div class='value'>" + current_song + "</div>";
  html += "</div>";
  if (current_artist != "") {
    html += "<div class='info'>";
    html += "<div class='label'>ARTIST</div>";
    html += "<div class='value'>" + current_artist + "</div>";
    html += "</div>";
  }
  html += "<button class='button' onclick='refresh()'>Refresh Now</button>";
  html += "<div class='status'>Auto-refreshing every 10 seconds</div>";
  html += "</div>";
  html += "</body></html>";
  
  server.send(200, "text/html", html);
}

void handleRefresh() {
  getCurrentlyPlaying();
  server.send(200, "text/plain", "Refreshed");
}