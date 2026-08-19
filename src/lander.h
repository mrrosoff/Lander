// Shared declarations for the Lander firmware. Split across:
//   main.cpp    — system + data fetch + backlight/battery + setup/loop
//   display.cpp — clock/console UI, weather icons
#pragma once

#include <Arduino.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

// ---------- shared types ----------
struct Location {
  String city;
  String region;
  float  latitude  = 0.0f;
  float  longitude = 0.0f;
  bool   valid     = false;
};

struct Weather {
  float   temperature_f   = 0.0f;
  float   humidity_pct    = 0.0f;
  float   cloud_cover_pct = 0.0f;
  float   wind_mph        = 0.0f;
  int     weather_code    = -1;
  String  sunrise_hhmm;
  String  sunset_hhmm;
  long    utc_offset_secs = 0;
  bool    valid           = false;
};

struct IndoorReading {
  float temperature_f = 0.0f;
  float humidity_pct  = 0.0f;
  bool  valid         = false;
};

// ---------- palette ----------
// bhoite's day palette
constexpr uint16_t COLOR_PINK    = 0xf816;
constexpr uint16_t COLOR_ORANGE  = 0xe469;
constexpr uint16_t COLOR_DIVIDER = 0x8c71;
constexpr uint16_t COLOR_BATT    = 0x3d9f;  // light blue

// ---------- shared objects (defined in main.cpp) ----------
extern Adafruit_ST7789 tft;

// ---------- shared state (defined in main.cpp) ----------
extern Location      g_location;
extern Weather       g_weather;
extern IndoorReading g_indoor;
extern bool g_ntp_configured;
extern bool g_static_drawn;
extern bool g_sensor_ok;
extern int  g_battery_pct;       // -1 until first read
extern bool g_usb_seen;

// ---------- display.cpp ----------
void drawStaticMiddle();
void drawTimeConsole();
void drawTemperatureConsole();
void drawSunTimes();
void renderAll();
const uint16_t* iconForWeatherCode(int code);
