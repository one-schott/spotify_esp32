// pinout.h - Hardware pin definitions
#ifndef PINOUT_H
#define PINOUT_H

// RGB LED Pins
#define LED_RED_PIN 25
#define LED_GREEN_PIN 26
#define LED_BLUE_PIN 27

// Button Pins
#define BUTTON_TOP_PIN 18      // Previous track
#define BUTTON_BOTTOM_PIN 23   // Next track

// OLED Display Pins (I2C)
#define OLED_SDA_PIN 21  // Standard I2C SDA pin
#define OLED_SCL_PIN 22  // Standard I2C SCL pin
#define OLED_I2C_ADDRESS 0x3C

// Display dimensions
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 32

#endif