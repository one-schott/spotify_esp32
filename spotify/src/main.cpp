#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <WebServer.h>
#include <ArduinoJson.h>
#include <base64.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Preferences.h>
#include "secrets.h"
#include "pinout.h"

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
String current_album = "";
String track_uri = "";
int track_duration_ms = 0;
int track_progress_ms = 0;
bool is_playing = true;
bool is_shuffled = false;
String repeat_state = "off"; // off, track, context
unsigned long progress_last_updated = 0;
unsigned long last_token_refresh = 0;
unsigned long last_song_check = 0;
const unsigned long token_refresh_interval = 3000000; // 50 minutes
const unsigned long song_check_interval = 2000; // 2 seconds (faster updates)

// Scrolling text variables
int scroll_position = 0;
unsigned long last_scroll_update = 0;
const unsigned long scroll_interval = 250; // Smoother scrolling

// Button state tracking
unsigned long last_button_press = 0;
const unsigned long button_debounce = 300; // Reduced for better responsiveness
unsigned long button_press_start[2] = {0, 0}; // Track when buttons were first pressed
bool button_was_pressed[2] = {false, false};
const unsigned long long_press_duration = 1000; // 1 second for long press

// LED pulsing for paused state
unsigned long last_pulse_update = 0;
int pulse_brightness = 0;
int pulse_direction = 1;

// Stats tracking
Preferences preferences;
unsigned long session_start_time = 0;
int songs_played_this_session = 0;
int total_songs_played = 0;
int skips_this_session = 0;

// WiFi reconnection
unsigned long last_wifi_check = 0;
const unsigned long wifi_check_interval = 30000; // Check WiFi every 30 seconds

// Error state tracking
int consecutive_api_failures = 0;
const int max_api_failures = 5;
bool error_state = false;

WebServer server(80);
WiFiClientSecure client;
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// Function declarations
void connectWiFi();
void checkWiFiConnection();
bool refreshAccessToken();
bool getCurrentlyPlaying();
void skipToNext();
void skipToPrevious();
void togglePlayPause();
void toggleShuffle();
void cycleRepeatMode();
void updateDisplay();
void setLEDColor(int r, int g, int b);
void updateLED();
void handleRoot();
void handleRefresh();
void handleControl();
void handleStats();
void checkButtons();
void saveStats();
void loadStats();
String formatTime(int ms);
void displayError(String error);

void setup() {
  Serial.begin(115200);
  
  // Load saved stats
  preferences.begin("spotify", false);
  loadStats();
  
  // Setup RGB LED pins
  pinMode(LED_RED_PIN, OUTPUT);
  pinMode(LED_GREEN_PIN, OUTPUT);
  pinMode(LED_BLUE_PIN, OUTPUT);
  setLEDColor(0, 0, 0);
  
  // Setup button pins (active HIGH - connected to 3.3V)
  pinMode(BUTTON_TOP_PIN, INPUT_PULLDOWN);
  pinMode(BUTTON_BOTTOM_PIN, INPUT_PULLDOWN);
  
  // Initialize I2C for OLED
  Wire.begin(OLED_SDA_PIN, OLED_SCL_PIN);
  
  // Initialize OLED display
  if(!display.begin(SSD1306_SWITCHCAPVCC, OLED_I2C_ADDRESS)) {
    Serial.println("SSD1306 allocation failed");
  } else {
    Serial.println("OLED initialized!");
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println("Spotify Display");
    display.println("Initializing...");
    display.display();
  }
  
  Serial.println("\n\n=================================");
  Serial.println("Spotify Now Playing - ESP32");
  Serial.println("Enhanced Edition v2.0");
  Serial.println("=================================");
  
  connectWiFi();
  
  if (WiFi.status() == WL_CONNECTED) {
    client.setInsecure();
    
    // Get initial access token
    if (refreshAccessToken()) {
      Serial.println("✓ Access token obtained");
      getCurrentlyPlaying();
      updateDisplay();
      session_start_time = millis();
    } else {
      Serial.println("✗ Failed to get access token");
      displayError("Auth Failed");
    }
  } else {
    displayError("WiFi Failed");
  }
  
  // Setup web server routes
  server.on("/", handleRoot);
  server.on("/refresh", handleRefresh);
  server.on("/control", handleControl);
  server.on("/stats", handleStats);
  server.onNotFound([]() {
    server.send(404, "text/plain", "Not Found");
  });
  server.begin();
  
  Serial.println("✓ Web server started");
  Serial.print("  → http://");
  Serial.println(WiFi.localIP());
  Serial.println("=================================\n");
}

void loop() {
  server.handleClient();
  checkButtons();
  checkWiFiConnection();
  
  unsigned long current_time = millis();
  
  // Refresh access token if needed
  if (current_time - last_token_refresh > token_refresh_interval) {
    Serial.println("Refreshing access token...");
    refreshAccessToken();
  }
  
  // Check currently playing song
  if (current_time - last_song_check > song_check_interval && !error_state) {
    getCurrentlyPlaying();
  }
  
  // Update scrolling display
  if (current_time - last_scroll_update > scroll_interval) {
    updateDisplay();
    last_scroll_update = current_time;
  }
  
  // Update LED (pulsing effect when paused)
  updateLED();
}

void connectWiFi() {
  Serial.print("Connecting to WiFi: ");
  Serial.println(ssid);
  
  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  
  int attempts = 0;
  while (WiFi.status() != WL_CONNECTED && attempts < 60) {
    delay(500);
    Serial.print(".");
    attempts++;
    
    if (attempts % 20 == 0) {
      Serial.println();
      Serial.print("Still trying... ");
    }
  }
  
  Serial.println();
  
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("✓ WiFi connected!");
    Serial.print("  IP: ");
    Serial.println(WiFi.localIP());
    Serial.print("  Signal: ");
    Serial.print(WiFi.RSSI());
    Serial.println(" dBm");
  } else {
    Serial.println("✗ WiFi connection failed!");
  }
}

void checkWiFiConnection() {
  unsigned long current_time = millis();
  
  if (current_time - last_wifi_check > wifi_check_interval) {
    last_wifi_check = current_time;
    
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFi disconnected! Reconnecting...");
      displayError("WiFi Lost");
      WiFi.reconnect();
      delay(5000);
      
      if (WiFi.status() == WL_CONNECTED) {
        Serial.println("✓ WiFi reconnected");
        error_state = false;
      }
    }
  }
}

bool refreshAccessToken() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("No WiFi connection");
    return false;
  }
  
  Serial.println("Requesting new access token...");
  
  if (!client.connect("accounts.spotify.com", 443)) {
    Serial.println("Connection to Spotify failed");
    consecutive_api_failures++;
    return false;
  }
  
  String auth = String(spotify_client_id) + ":" + String(spotify_client_secret);
  String auth_encoded = base64::encode(auth);
  String body = "grant_type=refresh_token&refresh_token=" + String(spotify_refresh_token);
  
  client.println("POST /api/token HTTP/1.1");
  client.println("Host: accounts.spotify.com");
  client.println("Authorization: Basic " + auth_encoded);
  client.println("Content-Type: application/x-www-form-urlencoded");
  client.print("Content-Length: ");
  client.println(body.length());
  client.println("Connection: close");
  client.println();
  client.println(body);
  
  while (client.connected() && !client.available()) {
    delay(10);
  }
  
  bool headers_end = false;
  while (client.available() && !headers_end) {
    String line = client.readStringUntil('\n');
    if (line == "\r") {
      headers_end = true;
    }
  }
  
  String response = client.readString();
  client.stop();
  
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, response);
  
  if (error) {
    Serial.print("JSON parsing failed: ");
    Serial.println(error.c_str());
    consecutive_api_failures++;
    return false;
  }
  
  if (doc["access_token"].is<const char*>()) {
    access_token = doc["access_token"].as<String>();
    last_token_refresh = millis();
    consecutive_api_failures = 0;
    Serial.println("✓ Access token refreshed");
    return true;
  }
  
  consecutive_api_failures++;
  return false;
}

bool getCurrentlyPlaying() {
  static unsigned long last_error_print = 0;
  
  if (access_token.isEmpty()) {
    if (millis() - last_error_print > 10000) {
      Serial.println("No access token available");
      last_error_print = millis();
    }
    return false;
  }
  
  if (WiFi.status() != WL_CONNECTED) {
    return false;
  }
  
  if (!client.connect("api.spotify.com", 443)) {
    consecutive_api_failures++;
    if (consecutive_api_failures >= max_api_failures) {
      error_state = true;
      displayError("API Error");
    }
    return false;
  }
  
  client.println("GET /v1/me/player/currently-playing HTTP/1.1");
  client.println("Host: api.spotify.com");
  client.println("Authorization: Bearer " + access_token);
  client.println("Connection: close");
  client.println();
  
  while (client.connected() && !client.available()) {
    delay(10);
  }
  
  String status_line = client.readStringUntil('\n');
  int status_code = 0;
  if (status_line.indexOf("HTTP/1.1") >= 0) {
    status_code = status_line.substring(9, 12).toInt();
  }
  
  bool headers_end = false;
  while (client.available() && !headers_end) {
    String line = client.readStringUntil('\n');
    if (line == "\r") {
      headers_end = true;
    }
  }
  
  if (status_code == 204) {
    String old_song = current_song;
    current_song = "Nothing playing";
    current_artist = "";
    current_album = "";
    track_uri = "";
    track_duration_ms = 0;
    track_progress_ms = 0;
    is_playing = false;
    if (old_song != current_song) {
      updateDisplay();
      setLEDColor(0, 0, 0);
    }
    client.stop();
    last_song_check = millis();
    consecutive_api_failures = 0;
    return true;
  }
  
  String response = client.readString();
  client.stop();
  
  if (response.isEmpty()) {
    consecutive_api_failures++;
    last_song_check = millis();
    return false;
  }
  
  JsonDocument doc;
  DeserializationError error = deserializeJson(doc, response);
  
  if (error) {
    consecutive_api_failures++;
    last_song_check = millis();
    return false;
  }
  
  if (doc["item"].is<JsonObject>()) {
    String new_song = doc["item"]["name"].as<String>();
    String new_artist = doc["item"]["artists"][0]["name"].as<String>();
    String new_album = doc["item"]["album"]["name"].as<String>();
    String new_uri = doc["item"]["uri"].as<String>();
    int new_duration = doc["item"]["duration_ms"].as<int>();
    int new_progress = doc["progress_ms"].as<int>();
    bool new_is_playing = doc["is_playing"].as<bool>();
    
    // Get shuffle and repeat states
    if (doc["shuffle_state"].is<bool>()) {
      is_shuffled = doc["shuffle_state"].as<bool>();
    }
    if (doc["repeat_state"].is<const char*>()) {
      repeat_state = doc["repeat_state"].as<String>();
    }
    
    if (new_song != current_song || new_artist != current_artist) {
      // New song detected
      current_song = new_song;
      current_artist = new_artist;
      current_album = new_album;
      track_uri = new_uri;
      track_duration_ms = new_duration;
      track_progress_ms = new_progress;
      is_playing = new_is_playing;
      progress_last_updated = millis();
      scroll_position = 0;
      
      songs_played_this_session++;
      total_songs_played++;
      saveStats();
      
      Serial.println("\n♪ Now Playing:");
      Serial.println("  Song:   " + current_song);
      Serial.println("  Artist: " + current_artist);
      Serial.println("  Album:  " + current_album);
      Serial.println("  Duration: " + formatTime(track_duration_ms));
      
      updateDisplay();
      
      // Cycle LED color
      static int song_count = 0;
      song_count++;
      int color_mode = song_count % 3;
      if (color_mode == 0) {
        setLEDColor(25, 0, 0);
      } else if (color_mode == 1) {
        setLEDColor(0, 25, 0);
      } else {
        setLEDColor(0, 0, 25);
      }
    } else {
      // Same song, update progress and play state
      track_progress_ms = new_progress;
      is_playing = new_is_playing;
      progress_last_updated = millis();
    }
    
    consecutive_api_failures = 0;
    error_state = false;
  } else {
    current_song = "Nothing playing";
    current_artist = "";
    current_album = "";
    track_uri = "";
    track_duration_ms = 0;
    track_progress_ms = 0;
  }
  
  last_song_check = millis();
  return true;
}

void skipToNext() {
  if (access_token.isEmpty() || WiFi.status() != WL_CONNECTED) return;
  
  Serial.println("⏭️  Skipping to next track");
  
  if (!client.connect("api.spotify.com", 443)) return;
  
  client.println("POST /v1/me/player/next HTTP/1.1");
  client.println("Host: api.spotify.com");
  client.println("Authorization: Bearer " + access_token);
  client.println("Content-Length: 0");
  client.println("Connection: close");
  client.println();
  
  while (client.connected() && !client.available()) {
    delay(10);
  }
  
  String status_line = client.readStringUntil('\n');
  client.stop();
  
  skips_this_session++;
  saveStats();
  
  delay(500);
  getCurrentlyPlaying();
}

void skipToPrevious() {
  if (access_token.isEmpty() || WiFi.status() != WL_CONNECTED) return;
  
  Serial.println("⏮️  Skipping to previous track");
  
  if (!client.connect("api.spotify.com", 443)) return;
  
  client.println("POST /v1/me/player/previous HTTP/1.1");
  client.println("Host: api.spotify.com");
  client.println("Authorization: Bearer " + access_token);
  client.println("Content-Length: 0");
  client.println("Connection: close");
  client.println();
  
  while (client.connected() && !client.available()) {
    delay(10);
  }
  
  String status_line = client.readStringUntil('\n');
  client.stop();
  
  skips_this_session++;
  saveStats();
  
  delay(500);
  getCurrentlyPlaying();
}

void togglePlayPause() {
  if (access_token.isEmpty() || WiFi.status() != WL_CONNECTED) return;
  
  String endpoint = is_playing ? "pause" : "play";
  Serial.println(is_playing ? "⏸️  Pausing" : "▶️  Playing");
  
  if (!client.connect("api.spotify.com", 443)) return;
  
  client.println("PUT /v1/me/player/" + endpoint + " HTTP/1.1");
  client.println("Host: api.spotify.com");
  client.println("Authorization: Bearer " + access_token);
  client.println("Content-Length: 0");
  client.println("Connection: close");
  client.println();
  
  while (client.connected() && !client.available()) {
    delay(10);
  }
  
  client.stop();
  
  delay(300);
  getCurrentlyPlaying();
}

void toggleShuffle() {
  if (access_token.isEmpty() || WiFi.status() != WL_CONNECTED) return;
  
  bool new_shuffle = !is_shuffled;
  Serial.println(new_shuffle ? "🔀 Shuffle ON" : "➡️  Shuffle OFF");
  
  if (!client.connect("api.spotify.com", 443)) return;
  
  client.println("PUT /v1/me/player/shuffle?state=" + String(new_shuffle ? "true" : "false") + " HTTP/1.1");
  client.println("Host: api.spotify.com");
  client.println("Authorization: Bearer " + access_token);
  client.println("Content-Length: 0");
  client.println("Connection: close");
  client.println();
  
  while (client.connected() && !client.available()) {
    delay(10);
  }
  
  client.stop();
  
  delay(300);
  getCurrentlyPlaying();
}

void cycleRepeatMode() {
  if (access_token.isEmpty() || WiFi.status() != WL_CONNECTED) return;
  
  String new_repeat = "off";
  if (repeat_state == "off") new_repeat = "context";
  else if (repeat_state == "context") new_repeat = "track";
  else new_repeat = "off";
  
  Serial.println("🔁 Repeat: " + new_repeat);
  
  if (!client.connect("api.spotify.com", 443)) return;
  
  client.println("PUT /v1/me/player/repeat?state=" + new_repeat + " HTTP/1.1");
  client.println("Host: api.spotify.com");
  client.println("Authorization: Bearer " + access_token);
  client.println("Content-Length: 0");
  client.println("Connection: close");
  client.println();
  
  while (client.connected() && !client.available()) {
    delay(10);
  }
  
  client.stop();
  
  delay(300);
  getCurrentlyPlaying();
}

void updateDisplay() {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  
  if (current_song == "Nothing playing" || current_song == "Not playing") {
    display.setTextSize(2);
    display.setCursor(10, 4);
    display.println("No Song");
    
    // Show session stats
    display.setTextSize(1);
    display.setCursor(0, 22);
    display.print("Played: ");
    display.print(songs_played_this_session);
  } else {
    int max_chars = 21;
    
    // Line 1: Song name (scrolling if needed)
    display.setCursor(0, 0);
    if (current_song.length() > max_chars) {
      String padded_song = current_song + "   ";
      int total_len = padded_song.length();
      String display_text = "";
      
      for (int i = 0; i < max_chars; i++) {
        display_text += padded_song[(scroll_position + i) % total_len];
      }
      display.println(display_text);
      scroll_position++;
      if (scroll_position >= total_len) {
        scroll_position = 0;
      }
    } else {
      display.println(current_song);
    }
    
    // Line 2: Artist name (scrolling if needed)
    display.setCursor(0, 10);
    if (current_artist.length() > max_chars) {
      String padded_artist = current_artist + "   ";
      int total_len = padded_artist.length();
      String display_text = "";
      
      for (int i = 0; i < max_chars; i++) {
        display_text += padded_artist[(scroll_position + i) % total_len];
      }
      display.println(display_text);
    } else {
      display.println(current_artist);
    }
    
    // Status icons (shuffle, repeat) on line 3
    display.setCursor(0, 20);
    if (is_shuffled) display.print("S ");
    if (repeat_state == "track") display.print("R1 ");
    else if (repeat_state == "context") display.print("R ");
    
    // Play/pause icon and progress bar on bottom line
    if (track_duration_ms > 0) {
      unsigned long time_elapsed = millis() - progress_last_updated;
      int estimated_progress = track_progress_ms;
      
      if (is_playing) {
        estimated_progress += time_elapsed;
      }
      
      if (estimated_progress > track_duration_ms) {
        estimated_progress = track_duration_ms;
      }
      
      // Play/pause icon (left 12 pixels)
      display.setCursor(0, 24);
      display.setTextSize(1);
      display.print(is_playing ? ">" : "||");
      
      // Progress bar (remaining space)
      int bar_x = 14;
      int bar_width = 114;  // 128 - 14 = 114 pixels remaining
      int bar_height = 6;
      int bar_y = 25;
      
      int filled_width = (estimated_progress * bar_width) / track_duration_ms;
      
      display.drawRect(bar_x, bar_y, bar_width, bar_height, SSD1306_WHITE);
      
      if (filled_width > 0) {
        display.fillRect(bar_x + 1, bar_y + 1, filled_width - 2, bar_height - 2, SSD1306_WHITE);
      }
    }
  }
  
  display.display();
}

void setLEDColor(int r, int g, int b) {
  analogWrite(LED_RED_PIN, r);
  analogWrite(LED_GREEN_PIN, g);
  analogWrite(LED_BLUE_PIN, b);
}

void updateLED() {
  if (!is_playing && current_song != "Nothing playing") {
    // Pulsing effect when paused
    unsigned long current_time = millis();
    if (current_time - last_pulse_update > 30) {
      last_pulse_update = current_time;
      
      pulse_brightness += pulse_direction * 2;
      
      if (pulse_brightness >= 25) {
        pulse_brightness = 25;
        pulse_direction = -1;
      } else if (pulse_brightness <= 5) {
        pulse_brightness = 5;
        pulse_direction = 1;
      }
      
      // Pulse with current color (maintain hue)
      static int last_color_mode = 0;
      int color_mode = last_color_mode;
      
      if (color_mode == 0) {
        setLEDColor(pulse_brightness, 0, 0);
      } else if (color_mode == 1) {
        setLEDColor(0, pulse_brightness, 0);
      } else {
        setLEDColor(0, 0, pulse_brightness);
      }
    }
  }
}

void checkButtons() {
  unsigned long current_time = millis();
  
  int top_state = digitalRead(BUTTON_TOP_PIN);
  int bottom_state = digitalRead(BUTTON_BOTTOM_PIN);
  
  // Top button (Next track / Long press: Toggle shuffle)
  if (top_state == HIGH) {
    if (!button_was_pressed[0]) {
      button_press_start[0] = current_time;
      button_was_pressed[0] = true;
    } else if (current_time - button_press_start[0] > long_press_duration) {
      // Long press detected
      Serial.println(">>> TOP LONG PRESS - Toggle Shuffle");
      toggleShuffle();
      button_was_pressed[0] = false; // Prevent repeat
      delay(500); // Debounce
    }
  } else {
    if (button_was_pressed[0] && current_time - button_press_start[0] < long_press_duration) {
      // Short press
      if (current_time - last_button_press > button_debounce) {
        Serial.println(">>> TOP BUTTON - Next track");
        skipToNext();
        last_button_press = current_time;
      }
    }
    button_was_pressed[0] = false;
  }
  
  // Bottom button (Previous track / Long press: Toggle repeat / Double press: Play/Pause)
  if (bottom_state == HIGH) {
    if (!button_was_pressed[1]) {
      button_press_start[1] = current_time;
      button_was_pressed[1] = true;
    } else if (current_time - button_press_start[1] > long_press_duration) {
      // Long press detected
      Serial.println(">>> BOTTOM LONG PRESS - Cycle Repeat");
      cycleRepeatMode();
      button_was_pressed[1] = false;
      delay(500);
    }
  } else {
    if (button_was_pressed[1] && current_time - button_press_start[1] < long_press_duration) {
      // Check for double press
      static unsigned long last_bottom_press = 0;
      if (current_time - last_bottom_press < 400) {
        // Double press
        Serial.println(">>> BOTTOM DOUBLE PRESS - Play/Pause");
        togglePlayPause();
        last_bottom_press = 0;
      } else {
        // Single press - wait to see if double
        if (current_time - last_bottom_press > 400) {
          Serial.println(">>> BOTTOM BUTTON - Previous track");
          skipToPrevious();
          last_button_press = current_time;
        }
        last_bottom_press = current_time;
      }
    }
    button_was_pressed[1] = false;
  }
}

void saveStats() {
  preferences.putInt("total_songs", total_songs_played);
}

void loadStats() {
  total_songs_played = preferences.getInt("total_songs", 0);
  Serial.print("Loaded stats - Total songs: ");
  Serial.println(total_songs_played);
}

String formatTime(int ms) {
  int seconds = ms / 1000;
  int minutes = seconds / 60;
  int secs = seconds % 60;
  char buffer[10];
  sprintf(buffer, "%d:%02d", minutes, secs);
  return String(buffer);
}

void displayError(String error) {
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);
  display.setCursor(0, 0);
  display.println("ERROR:");
  display.setTextSize(2);
  display.setCursor(0, 12);
  display.println(error);
  display.display();
  
  // Red LED for error
  setLEDColor(25, 0, 0);
}

void handleRoot() {
  String html = "<!DOCTYPE html><html><head>";
  html += "<meta charset='UTF-8'>";
  html += "<meta name='viewport' content='width=device-width, initial-scale=1.0'>";
  html += "<title>Spotify Controller</title>";
  html += "<style>";
  html += "body { font-family: 'Segoe UI', Tahoma, Geneva, Verdana, sans-serif; ";
  html += "background: linear-gradient(135deg, #1DB954 0%, #191414 100%); ";
  html += "color: white; margin: 0; padding: 20px; min-height: 100vh; }";
  html += ".container { background: rgba(0,0,0,0.7); padding: 30px; ";
  html += "border-radius: 20px; max-width: 500px; margin: 0 auto; }";
  html += "h1 { text-align: center; margin: 0 0 20px 0; font-size: 2em; }";
  html += ".info { margin: 15px 0; }";
  html += ".label { color: #1DB954; font-weight: bold; font-size: 0.9em; }";
  html += ".value { font-size: 1.3em; }";
  html += ".controls { display: grid; grid-template-columns: 1fr 1fr; gap: 10px; margin: 20px 0; }";
  html += ".btn { background: #1DB954; color: white; border: none; ";
  html += "padding: 15px; font-size: 1em; border-radius: 10px; cursor: pointer; }";
  html += ".btn:hover { background: #1ed760; }";
  html += ".stats { background: rgba(255,255,255,0.1); padding: 15px; border-radius: 10px; margin: 20px 0; }";
  html += ".progress { background: rgba(255,255,255,0.2); height: 8px; border-radius: 4px; margin: 10px 0; }";
  html += ".progress-bar { background: #1DB954; height: 100%; border-radius: 4px; transition: width 0.3s; }";
  html += "</style>";
  html += "<script>";
  html += "function control(action) { fetch('/control?action=' + action).then(() => setTimeout(() => location.reload(), 500)); }";
  html += "setInterval(() => location.reload(), 5000);";
  html += "</script>";
  html += "</head><body>";
  html += "<div class='container'>";
  html += "<h1>🎵 Spotify Controller</h1>";
  
  if (current_song != "Nothing playing" && current_song != "Not playing") {
    html += "<div class='info'><div class='label'>SONG</div><div class='value'>" + current_song + "</div></div>";
    html += "<div class='info'><div class='label'>ARTIST</div><div class='value'>" + current_artist + "</div></div>";
    html += "<div class='info'><div class='label'>ALBUM</div><div class='value'>" + current_album + "</div></div>";
    
    if (track_duration_ms > 0) {
      unsigned long time_elapsed = millis() - progress_last_updated;
      int estimated_progress = track_progress_ms + (is_playing ? time_elapsed : 0);
      if (estimated_progress > track_duration_ms) estimated_progress = track_duration_ms;
      int progress_percent = (estimated_progress * 100) / track_duration_ms;
      
      html += "<div class='progress'><div class='progress-bar' style='width:" + String(progress_percent) + "%'></div></div>";
      html += "<div style='text-align:center;font-size:0.9em;'>" + formatTime(estimated_progress) + " / " + formatTime(track_duration_ms) + "</div>";
    }
    
    html += "<div class='controls'>";
    html += "<button class='btn' onclick='control(\"prev\")'>⏮️ Previous</button>";
    html += "<button class='btn' onclick='control(\"next\")'>Next ⏭️</button>";
    html += "<button class='btn' onclick='control(\"playpause\")'>" + String(is_playing ? "⏸️ Pause" : "▶️ Play") + "</button>";
    html += "<button class='btn' onclick='control(\"shuffle\")'>" + String(is_shuffled ? "🔀 Shuffle ON" : "➡️ Shuffle OFF") + "</button>";
    html += "</div>";
  } else {
    html += "<div style='text-align:center;font-size:1.5em;margin:40px 0;'>Nothing playing</div>";
  }
  
  html += "<div class='stats'>";
  html += "<div class='label'>SESSION STATS</div>";
  html += "<div>Songs: " + String(songs_played_this_session) + " | Skips: " + String(skips_this_session) + "</div>";
  html += "<div>Total: " + String(total_songs_played) + " songs</div>";
  html += "<div>Uptime: " + formatTime(millis() - session_start_time) + "</div>";
  html += "</div>";
  
  html += "<div style='text-align:center;margin-top:20px;font-size:0.8em;color:#b3b3b3;'>Auto-refresh: 5s | ";
  html += "Signal: " + String(WiFi.RSSI()) + " dBm</div>";
  html += "</div></body></html>";
  
  server.send(200, "text/html", html);
}

void handleRefresh() {
  getCurrentlyPlaying();
  server.send(200, "text/plain", "OK");
}

void handleControl() {
  if (server.hasArg("action")) {
    String action = server.arg("action");
    
    if (action == "next") {
      skipToNext();
    } else if (action == "prev") {
      skipToPrevious();
    } else if (action == "playpause") {
      togglePlayPause();
    } else if (action == "shuffle") {
      toggleShuffle();
    } else if (action == "repeat") {
      cycleRepeatMode();
    }
  }
  
  server.send(200, "text/plain", "OK");
}

void handleStats() {
  String json = "{";
  json += "\"current_song\":\"" + current_song + "\",";
  json += "\"current_artist\":\"" + current_artist + "\",";
  json += "\"is_playing\":" + String(is_playing ? "true" : "false") + ",";
  json += "\"songs_session\":" + String(songs_played_this_session) + ",";
  json += "\"skips_session\":" + String(skips_this_session) + ",";
  json += "\"total_songs\":" + String(total_songs_played) + ",";
  json += "\"uptime_ms\":" + String(millis() - session_start_time);
  json += "}";
  
  server.send(200, "application/json", json);
}