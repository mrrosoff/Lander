// Shared declarations for the Lander firmware. Split across:
//   main.cpp       — system + data fetch + backlight/battery + setup/loop
//   display.cpp    — clock/console UI, loading screen, weather icons
//   animations.cpp — sleep/wake transitions, first-boot landing,
//                    connectivity-trouble screen, science activities
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

struct Star { uint8_t x, y; };

// ---------- palette ----------
// bhoite's day palette
constexpr uint16_t COLOR_PINK    = 0xf816;
constexpr uint16_t COLOR_ORANGE  = 0xe469;
constexpr uint16_t COLOR_DIVIDER = 0x8c71;
constexpr uint16_t COLOR_BATT    = 0x3d9f;  // light blue
// dark-mode night palette: muted greens and purples for the quiet-hours screen
constexpr uint16_t NIGHT_GREEN   = 0x3E6F;  // soft mint (time)
constexpr uint16_t NIGHT_PURPLE  = 0x92D9;  // muted violet (date, AM/PM)
constexpr uint16_t NIGHT_MAGENTA = 0xBAD7;  // dusty magenta (city)
constexpr uint16_t NIGHT_TEXT    = 0x6BAE;  // grey-green (indoor readouts)
constexpr uint16_t NIGHT_MOON    = 0xCDFC;  // pale lavender (moon)
constexpr uint16_t NIGHT_SUN     = 0x8C8D;  // dim olive (sun times)

// ---------- science activities ----------
enum Activity : uint8_t {
  ACT_EXCAVATE, ACT_WEATHER, ACT_SOLAR, ACT_REACTOR,
  ACT_TRANSMIT, ACT_STARGAZE, ACT_DIAG, ACT_SEISMO, ACT_PANORAMA,
  ACT_COUNT
};
constexpr uint32_t ACTIVITY_RUN_MS      = 13000;             // animated portion
constexpr uint32_t ACTIVITY_HOLD_MS     = 2000;              // result hold -> ~15 s total
constexpr uint32_t ACTIVITY_GAP_MIN_MS  = 10UL * 60 * 1000;  // hard floor between any two
constexpr uint32_t ACTIVITY_IDLE_MIN_MS = 16UL * 60 * 1000;  // random idle interval, low
constexpr uint32_t ACTIVITY_IDLE_MAX_MS = 32UL * 60 * 1000;  // random idle interval, high
// Per-trigger cooldowns so a reactive activity can't crowd out the idle pool:
// SOLAR re-arms on every 2% climb (constant on a sunny day), WEATHER can
// oscillate between adjacent codes on a marginal day.
constexpr uint32_t SOLAR_COOLDOWN_MS    = 2UL * 60 * 60 * 1000;  // at most once / 2h
constexpr uint32_t WEATHER_COOLDOWN_MS  = 45UL * 60 * 1000;      // at most once / 45min

// ---------- shared objects (defined in main.cpp / display.cpp) ----------
extern Adafruit_ST7789 tft;
extern const Star      STARFIELD[];
extern const uint8_t   N_STARS;

// ---------- shared state (defined in main.cpp) ----------
extern Location      g_location;
extern Weather       g_weather;
extern IndoorReading g_indoor;
extern bool g_ntp_configured;
extern bool g_booted;            // true once the real UI is on screen
extern bool g_static_drawn;
extern bool g_loading_bg_drawn;
extern bool g_sensor_ok;
extern int  g_battery_pct;       // -1 until first read
extern bool g_usb_seen;
extern uint32_t g_next_activity_ms;
extern uint32_t g_last_activity_ms;

// ---------- display.cpp ----------
void drawStarfield();
void drawLoadingScreen(const char* line1, const char* line2, uint8_t frame);
void updateLoadingFrame(uint8_t frame);
void scrollLoadingStars();
void drawStaticMiddle();
void drawTimeConsole(bool night);
void drawTemperatureConsole(bool night);
void drawSunTimes(bool night);
void renderAll();
const uint16_t* iconForWeatherCode(int code);

// ---------- main.cpp ----------
bool isAwakeHour();

// ---------- animations.cpp ----------
void playSleepTransition();
void playWakeTransition();
void playLanding();
void playTrouble(const char* msg, int theme, uint32_t duration_ms = 3600);  // theme: 0 wifi, 1 location, 2 weather
void playActivity(uint8_t a);
void scheduleNextActivity();
uint8_t pickIdleActivity();
void afterActivity();
