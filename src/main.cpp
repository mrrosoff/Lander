// Lander firmware: WiFi -> ipinfo.io (geolocation) -> Open-Meteo (weather +
// sunrise/sunset + tz) -> NTP for the clock. UI is a port of Mohit Bhoite's
// Boron Lander layout (https://bhoite.com/sculptures/boron-lander).
// Code is split across main / display / animations / lander.h.
//
// Board: ESP32-S3 SuperMini (HW-747).
// Wiring (screen pin -> ESP32-S3 GPIO):
//   BLK -> GPIO 12 (PWM backlight)
//   CS  -> GPIO 11
//   DC  -> GPIO 10
//   RES -> GPIO 5
//   SDA -> GPIO 6  (MOSI)
//   SCL -> GPIO 7  (SCLK)
//   VCC -> 3V3
//   GND -> GND
//
// SHT31 indoor temp/humidity sensor (I2C, 0x44):
//   VIN -> 3V3 / GND -> GND / SDA -> GPIO 8 / SCL -> GPIO 9
//
// Battery voltage divider (2x 100k between TP4056 BAT+ and GND):
//   midpoint -> GPIO 4 (ADC)

#include <Arduino.h>
#include <SPI.h>
#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <FastLED.h>
#include <Adafruit_SHT31.h>
#include <time.h>

#include "secrets.h"
#include "lander.h"
#include "lander_logic.h"

// WiFi credentials come from secrets.h as a WIFI_NETWORKS list of
// WIFI_AP(ssid, pass) entries (see secrets.example.h). The strongest in-range
// network is used; add more by adding WIFI_AP(...) lines.
#ifndef WIFI_NETWORKS
#error "Define WIFI_NETWORKS in include/secrets.h (see include/secrets.example.h)"
#endif
struct WifiCred { const char* ssid; const char* pass; };
#define WIFI_AP(s, p) {s, p},
static const WifiCred WIFI_NETWORKS_LIST[] = { WIFI_NETWORKS };
#undef WIFI_AP
static constexpr size_t WIFI_NETWORKS_N = sizeof(WIFI_NETWORKS_LIST) / sizeof(*WIFI_NETWORKS_LIST);

constexpr int8_t  PIN_TFT_CS    = 11;
constexpr int8_t  PIN_TFT_DC    = 10;
constexpr int8_t  PIN_TFT_RST   = 5;
constexpr uint8_t PIN_BACKLIGHT = 12;
constexpr int8_t  PIN_TFT_MOSI  = 6;
constexpr int8_t  PIN_TFT_SCLK  = 7;
constexpr int8_t  PIN_I2C_SDA   = 8;
constexpr int8_t  PIN_I2C_SCL   = 9;
constexpr int8_t  PIN_BATT_ADC  = 4;

constexpr uint8_t  BACKLIGHT_PWM_CHANNEL = 0;
constexpr uint32_t BACKLIGHT_PWM_FREQ_HZ = 5000;
constexpr uint8_t  BACKLIGHT_PWM_BITS    = 8;
// Brightness rides a sunrise->solar-noon->sunset bell curve: brightest at
// midday, dimmest at the edges and overnight.
constexpr uint8_t  BACKLIGHT_DUTY_PEAK   = 28;   // ~11% of 255, at solar noon
constexpr uint8_t  BACKLIGHT_DUTY_FLOOR  = 6;    // ~2% of 255, at sunrise/sunset
constexpr uint8_t  BACKLIGHT_DUTY_NIGHT  = 4;    // ~1.5% of 255, quiet-hours night screen

constexpr uint8_t  PIN_LED          = 48;
constexpr uint32_t LED_PULSE_MS     = 4000;  // one breath in-and-out
constexpr uint32_t LED_PAUSE_MS     = 3000;  // rest between breaths
constexpr uint32_t LED_PERIOD_MS    = LED_PULSE_MS + LED_PAUSE_MS;
// Breath runs 0..ceiling in raw channel units (global brightness = full, so we
// get the LED's complete 8-bit range). The ceiling tracks the backlight
// sun-curve, so the LED dims with the screen and is brightest at midday.
constexpr uint8_t  LED_CEIL         = 40;     // brightest point of a breath at midday peak
constexpr float    LED_GAMMA        = 0.6f;   // <1 rushes the dim end of the fade so it doesn't step

constexpr uint32_t WIFI_TIMEOUT_MS    = 30000;
constexpr uint32_t WEATHER_REFRESH_MS = 30UL * 60UL * 1000UL;  // awake hours only
constexpr uint32_t SENSOR_POLL_MS     = 5000UL;
constexpr uint32_t BATTERY_POLL_MS    = 30000UL;
constexpr uint32_t RETRY_BACKOFF_MS   = 30UL * 1000UL;
constexpr uint32_t NTP_RETRY_MS       = 15UL * 1000UL;  // re-sync clock while stuck on SYNCING

// TEMP: set to 1 to play the sleep + wake transitions once at boot. Back to 0
// to remove.
#define ANIM_TEST_AT_BOOT 0
// TEMP: set to 1 to slowly loop through every science activity at boot for preview.
#define ACTIVITY_TEST_AT_BOOT 0
// TEMP: set to 1 to preview the connectivity error animations at boot.
#define TROUBLE_TEST_AT_BOOT 0

// ---------- objects + globals (the externs declared in lander.h) ----------
Adafruit_ST7789 tft(PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST);
Adafruit_SHT31  sensor;
CRGB g_led[1];

Location      g_location;
Weather       g_weather;
IndoorReading g_indoor;
bool g_ntp_configured   = false;
bool g_booted           = false;
bool g_static_drawn     = false;
bool g_loading_bg_drawn = false;
bool g_sensor_ok        = false;
int  g_battery_pct      = -1;
bool g_usb_seen         = false;
uint32_t g_next_activity_ms = 0;
uint32_t g_last_activity_ms = 0;

// loop-local timers (not shared across files)
static uint32_t g_last_weather_fetch_ms = 0;
static uint32_t g_last_sensor_poll_ms   = 0;
static uint32_t g_last_battery_poll_ms  = 0;
static uint32_t g_last_ntp_retry_ms     = 0;

// Fires on USB bus reset (cable connected/enumerated). Stays true for the whole
// boot session so the USB indicator persists even if the serial port is closed.
static void onUsbBusReset(void*, esp_event_base_t, int32_t, void*) {
  g_usb_seen = true;
}

// ---------- HTTPS GET ----------
static bool httpsGet(const String& url, String& outBody) {
  WiFiClientSecure client;
  client.setInsecure();
  HTTPClient https;
  if (!https.begin(client, url)) return false;
  int code = https.GET();
  if (code != HTTP_CODE_OK) { https.end(); return false; }
  outBody = https.getString();
  https.end();
  return true;
}

static String hhmmFromIso(const char* iso) {
  if (!iso || strlen(iso) < 16) return String("--:--");
  return String(iso).substring(11, 16);
}

// Rear LED "breath": a smooth in-and-out pulse over LED_PULSE_MS, then a quiet
// rest of LED_PAUSE_MS before the next one. Returns 0..1.
static float ledBreath() {
  uint32_t phase = millis() % LED_PERIOD_MS;
  if (phase >= LED_PULSE_MS) return 0.0f;          // resting between breaths
  float x = (float)phase / (float)LED_PULSE_MS;    // 0..1 across one breath
  return (1.0f - cosf(x * 2.0f * 3.14159f)) * 0.5f;
}

// ---------- WiFi / data fetch ----------
static bool connectWifi() {
  WiFi.mode(WIFI_STA);
  // The loading animation only takes over the screen on first boot; background
  // refreshes stay silent so the clock keeps showing.
  if (!g_booted) drawLoadingScreen("CONNECTING", "", 0);
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);

  // Scan asynchronously so the loading screen + LED keep animating instead of
  // freezing on a static "CONNECTING" frame during the (blocking) all-channel
  // scan. scanComplete() is <0 while running (-1) or pending/failed (-2); wait
  // until it reports a count, with a safety timeout.
  WiFi.scanNetworks(true);
  uint32_t scan_start = millis();
  uint8_t  scan_frame = 0;
  uint32_t scan_last  = 0;
  int found;
  while ((found = WiFi.scanComplete()) < 0 && millis() - scan_start < 8000) {
    if (!g_booted && millis() - scan_last >= 250) {
      updateLoadingFrame(++scan_frame);
      scan_last = millis();
    }
    if (!g_booted) scrollLoadingStars();
    g_led[0] = CRGB(0, 0, (uint8_t)(ledBreath() * LED_CEIL));
    FastLED.show();
    delay(50);
  }
  if (found < 0) found = 0;

  // Pick the strongest in-range network we have credentials for.
  const WifiCred* best = nullptr;
  int best_rssi = -1000;
  for (int i = 0; i < found; i++) {
    for (size_t k = 0; k < WIFI_NETWORKS_N; k++) {
      if (WiFi.RSSI(i) > best_rssi && WiFi.SSID(i) == WIFI_NETWORKS_LIST[k].ssid) {
        best_rssi = WiFi.RSSI(i);
        best = &WIFI_NETWORKS_LIST[k];
      }
    }
  }
  WiFi.scanDelete();
  if (!best) {
    if (!g_booted) playTrouble("NO NETWORKS", 0, RETRY_BACKOFF_MS);
    return false;
  }

  if (!g_booted) drawLoadingScreen("CONNECTING", best->ssid, 0);
  WiFi.begin(best->ssid, best->pass);
  uint32_t start = millis();
  uint8_t  anim_frame = 0;
  uint32_t last_anim  = 0;
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT_MS) {
    if (!g_booted && millis() - last_anim >= 250) {
      updateLoadingFrame(++anim_frame);
      last_anim = millis();
    }
    if (!g_booted) scrollLoadingStars();
    g_led[0] = CRGB(0, 0, (uint8_t)(ledBreath() * LED_CEIL));
    FastLED.show();
    delay(50);
  }
  if (WiFi.status() != WL_CONNECTED) {
    if (!g_booted) playTrouble("NO WIFI", 0, RETRY_BACKOFF_MS);
    return false;
  }
  Serial.printf("WiFi: %s RSSI=%d\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
  if (!g_booted) g_static_drawn = false;  // re-paint static band after status screen
  return true;
}

// Run a blocking network+parse job on the other core so the loading screen and
// LED keep animating on the main core while TLS/HTTP runs. The Arduino loop is
// on core 1, so the job is pinned to core 0 (where WiFi already lives).
struct FetchJob { bool (*work)(); volatile bool done; volatile bool ok; };
static void fetchTask(void* arg) {
  FetchJob* j = (FetchJob*)arg;
  j->ok = j->work();
  j->done = true;
  vTaskDelete(nullptr);
}
static bool runWithLoading(bool (*work)(), const char* label) {
  if (!g_booted) drawLoadingScreen(label, "", 0);
  FetchJob job{work, false, false};
  xTaskCreatePinnedToCore(fetchTask, "fetch", 16384, &job, 1, nullptr, 0);
  uint8_t frame = 0; uint32_t last = 0;
  while (!job.done) {
    if (!g_booted && millis() - last >= 200) { updateLoadingFrame(++frame); last = millis(); }
    if (!g_booted) scrollLoadingStars();
    g_led[0] = CRGB(0, 0, (uint8_t)(ledBreath() * LED_CEIL));
    FastLED.show();
    delay(30);
  }
  return job.ok;
}

static bool locationWork() {
  String body;
  if (!httpsGet("https://ipinfo.io/json", body)) return false;

  JsonDocument doc;
  if (deserializeJson(doc, body)) return false;

  g_location.city   = (const char*)(doc["city"]   | "");
  g_location.region = (const char*)(doc["region"] | "");

  // ipinfo.io packs coordinates as "lat,lon" in a single "loc" field.
  const char* loc = doc["loc"] | (const char*)nullptr;
  g_location.latitude = g_location.longitude = 0.0f;
  if (loc) {
    const char* comma = strchr(loc, ',');
    if (comma) {
      g_location.latitude  = atof(loc);
      g_location.longitude = atof(comma + 1);
    }
  }

  g_location.valid = g_location.city.length() > 0;

  if (!g_booted) g_static_drawn = false;
  return g_location.valid;
}
static bool fetchLocation() { return runWithLoading(locationWork, "LOCATING"); }

// Point SNTP at the pool and wait up to 5 s for the first sync while WiFi is
// up. Caller is responsible for having WiFi connected. Uses the offset from the
// last weather fetch so the clock lands in local time.
static bool syncNtp() {
  configTime(g_weather.utc_offset_secs, 0, "pool.ntp.org", "time.nist.gov");
  struct tm tmp;
  uint32_t t0 = millis();
  while (!getLocalTime(&tmp, 0) && millis() - t0 < 5000) delay(100);
  g_ntp_configured = getLocalTime(&tmp, 0);
  Serial.printf("NTP %s, offset=%ld sec\n",
                g_ntp_configured ? "synced" : "timeout", g_weather.utc_offset_secs);
  return g_ntp_configured;
}

static bool weatherWork() {
  char url[448];
  snprintf(url, sizeof(url),
           "https://api.open-meteo.com/v1/forecast"
           "?latitude=%.4f&longitude=%.4f"
           "&current=temperature_2m,relative_humidity_2m,weather_code,cloud_cover,wind_speed_10m"
           "&daily=sunrise,sunset"
           "&temperature_unit=fahrenheit&wind_speed_unit=mph&timezone=auto&forecast_days=1",
           g_location.latitude, g_location.longitude);

  String body;
  if (!httpsGet(url, body)) return false;

  JsonDocument doc;
  if (deserializeJson(doc, body)) return false;

  g_weather.temperature_f  = doc["current"]["temperature_2m"]      | 0.0f;
  g_weather.humidity_pct   = doc["current"]["relative_humidity_2m"]| 0.0f;
  g_weather.cloud_cover_pct= doc["current"]["cloud_cover"]         | 0.0f;
  g_weather.wind_mph       = doc["current"]["wind_speed_10m"]      | 0.0f;
  g_weather.weather_code   = doc["current"]["weather_code"]        | -1;
  g_weather.utc_offset_secs= doc["utc_offset_seconds"]             | 0L;

  const char* sr = doc["daily"]["sunrise"][0] | (const char*)nullptr;
  const char* ss = doc["daily"]["sunset"][0]  | (const char*)nullptr;
  g_weather.sunrise_hhmm = hhmmFromIso(sr);
  g_weather.sunset_hhmm  = hhmmFromIso(ss);

  g_weather.valid = g_weather.weather_code >= 0;

  if (g_weather.valid && !g_ntp_configured) syncNtp();

  Serial.printf("Weather: %.1fF wmo=%d\n", g_weather.temperature_f, g_weather.weather_code);
  if (!g_booted) g_static_drawn = false;
  return g_weather.valid;
}
static bool fetchWeather() {
  if (!g_location.valid) return false;
  return runWithLoading(weatherWork, "WEATHER");
}

static void wifiOff() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

// ---------- quiet-hours gating ----------
// The dark-mode night screen shows during the recipient's sleep window.
// Minutes-of-day boundaries so we can use half-hour edges. Fail-open: if we
// don't yet know the time, treat it as awake (day screen).
constexpr int QUIET_START_MIN = 1 * 60 +  0;  // 1:00 AM, inclusive
constexpr int QUIET_END_MIN   = 5 * 60 + 30;  // 5:30 AM, exclusive

bool isAwakeHour() {
  if (!g_ntp_configured) return true;
  struct tm now;
  if (!getLocalTime(&now, 0)) return true;
  int m = now.tm_hour * 60 + now.tm_min;
  return m < QUIET_START_MIN || m >= QUIET_END_MIN;
}

// ---------- sun-curve backlight ----------
static int minutesOfDayNow() {
  struct tm now;
  if (!g_ntp_configured || !getLocalTime(&now, 0)) return -1;
  return now.tm_hour * 60 + now.tm_min;
}

static int minutesFromHhmm(const String& s) {
  if (s.length() < 5 || s[2] != ':') return -1;
  return s.substring(0, 2).toInt() * 60 + s.substring(3, 5).toInt();
}

// During quiet hours the dim night screen stays up at NIGHT duty. Otherwise:
// floor outside daylight, and across daylight a raised-sine bell peaking at
// solar noon. Falls back to PEAK until time / sun data is known.
static uint8_t targetBacklightDuty() {
  if (!isAwakeHour()) return BACKLIGHT_DUTY_NIGHT;
  int t  = minutesOfDayNow();
  int sr = minutesFromHhmm(g_weather.sunrise_hhmm);
  int ss = minutesFromHhmm(g_weather.sunset_hhmm);
  if (t < 0 || sr < 0 || ss < 0 || ss <= sr) return BACKLIGHT_DUTY_PEAK;
  if (t <= sr || t >= ss) return BACKLIGHT_DUTY_FLOOR;
  float x    = (float)(t - sr) / (float)(ss - sr);  // 0..1 across daylight
  float bell = sinf(x * 3.14159265f);               // 0 at edges, 1 at noon
  return BACKLIGHT_DUTY_FLOOR +
         (uint8_t)((BACKLIGHT_DUTY_PEAK - BACKLIGHT_DUTY_FLOOR) * bell + 0.5f);
}

static void updateBacklight() {
  uint8_t duty = targetBacklightDuty();
  static int16_t current = -1;
  if ((int16_t)duty == current) return;
  bool was_off = current <= 0;
  if (was_off && duty > 0) tft.enableDisplay(true);
  ledcWrite(BACKLIGHT_PWM_CHANNEL, duty);
  if (!was_off && duty == 0) tft.enableDisplay(false);
  current = duty;
}

// ---------- battery ----------
// batteryPercentFromMv() lives in lander_logic.h (pure, unit-tested).

// The 2x100k divider (~50k source impedance) is higher than the ESP32-S3 ADC
// likes, so analogReadMilliVolts reads a few % low. Single-point fix: a full
// 4.2V pack read ~80% (~4000 mV), so scale the result up ~5%. Tune against the
// "Batt: N mV" serial log if a fully-charged pack doesn't land near 4200 mV.
constexpr float BATT_CAL = 1.05f;

static void pollBattery() {
  // Average 16 reads to smooth ADC noise; x2 undoes the divider, then calibrate.
  long sum = 0;
  for (int i = 0; i < 16; i++) sum += analogReadMilliVolts(PIN_BATT_ADC);
  int batt_mv = (int)((sum / 16) * 2 * BATT_CAL);
  int prev = g_battery_pct;
  g_battery_pct = batteryPercentFromMv(batt_mv);
  if (g_battery_pct != prev)   // only on change, not every poll
    Serial.printf("Batt: %d mV -> %d%%\n", batt_mv, g_battery_pct);
}

static void pollIndoor() {
  if (!g_sensor_ok) return;
  float t = sensor.readTemperature();
  float h = sensor.readHumidity();
  if (isnan(t) || isnan(h)) return;
  g_indoor.temperature_f = t * 9.0f / 5.0f + 32.0f;
  g_indoor.humidity_pct  = h;
  g_indoor.valid         = true;
}

// ---------- setup / loop ----------
void setup() {
  Serial.begin(115200);
  Serial.onEvent(ARDUINO_HW_CDC_BUS_RESET_EVENT, onUsbBusReset);
  delay(500);
  Serial.println("Lander boot");

  // 80 MHz is plenty for a clock + 30-min poll and WiFi still works; cuts the
  // continuous CPU current (the loop never sleeps).
  setCpuFrequencyMhz(80);

  FastLED.addLeds<NEOPIXEL, PIN_LED>(g_led, 1);
  // Full global brightness so the LED gets its complete 8-bit range; the actual
  // (dim) level is set per-frame via the LED_FLOOR..LED_CEIL band below.
  FastLED.setBrightness(255);
  g_led[0] = CRGB(0, 0, LED_CEIL);
  FastLED.show();

  ledcSetup(BACKLIGHT_PWM_CHANNEL, BACKLIGHT_PWM_FREQ_HZ, BACKLIGHT_PWM_BITS);
  ledcAttachPin(PIN_BACKLIGHT, BACKLIGHT_PWM_CHANNEL);
  ledcWrite(BACKLIGHT_PWM_CHANNEL, BACKLIGHT_DUTY_PEAK);

  SPI.begin(PIN_TFT_SCLK, -1, PIN_TFT_MOSI, PIN_TFT_CS);
  tft.init(240, 240, SPI_MODE0);
  tft.setSPISpeed(20000000);
  tft.setRotation(2);
  tft.setTextWrap(false);

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  g_sensor_ok = sensor.begin(0x44);
  if (!g_sensor_ok) Serial.println("SHT31 init FAILED");

  randomSeed(esp_random());
  scheduleNextActivity();

#if ANIM_TEST_AT_BOOT
  for (int i = 0; i < 2; i++) { playSleepTransition(); playWakeTransition(); }
#endif
#if ACTIVITY_TEST_AT_BOOT
  // Preview mode: slowly loop through every activity forever, with a short gap
  // between each so they're easy to review. Set the flag to 0 to boot normally.
  for (;;) {
    for (uint8_t a = 0; a < ACT_COUNT; a++) {
      playActivity(a);
      tft.fillScreen(ST77XX_BLACK);
      delay(2000);
    }
  }
#endif
#if TROUBLE_TEST_AT_BOOT
  playTrouble("NO WIFI", 0);
  playTrouble("NO FIX", 1);
  playTrouble("WX OFFLINE", 2);
#endif

  drawLoadingScreen("LANDER", "booting...", 0);
}

void loop() {
  bool awake = isAwakeHour();

  // Refresh weather only while awake — the night screen runs on cached data
  // with the radio off. The first fetch is always allowed so there's data.
  bool need_weather = !g_weather.valid ||
      (awake && (millis() - g_last_weather_fetch_ms) >= WEATHER_REFRESH_MS);

  if (need_weather) {
    // On failure, the backoff is spent *animating* the retry screen at boot
    // (never a frozen frame); once booted the clock stays up, so a silent wait
    // is fine. connectWifi() already animates its own failures at boot.
    if (!connectWifi()) { if (g_booted) delay(RETRY_BACKOFF_MS); return; }
    if (!g_location.valid && !fetchLocation()) {
      wifiOff();
      if (!g_booted) playTrouble("NO FIX", 1, RETRY_BACKOFF_MS);
      else           delay(RETRY_BACKOFF_MS);
      return;
    }
    if (fetchWeather()) {
      g_last_weather_fetch_ms = millis();
      if (!g_booted) playLanding();
      renderAll();
      g_booted = true;  // real UI is now live; suppress future loading screens
    } else {
      wifiOff();
      if (!g_booted) playTrouble("WX OFFLINE", 2, RETRY_BACKOFF_MS);
      else           delay(RETRY_BACKOFF_MS);
      return;
    }
    wifiOff();
  }

  // If weather came back but the clock never did, the UI is stuck on
  // "SYNCING...". Retry NTP on its own tight timer instead of riding the 30-min
  // weather cadence (WiFi is off between refreshes, so SNTP can't recover on its
  // own). Awake-only, to keep the radio dark during quiet hours.
  if (g_booted && awake && !g_ntp_configured && g_weather.valid &&
      (millis() - g_last_ntp_retry_ms) >= NTP_RETRY_MS) {
    g_last_ntp_retry_ms = millis();
    if (connectWifi()) {
      if (syncNtp()) drawTimeConsole(!awake);
      wifiOff();
    }
  }

  updateBacklight();

  bool night = !awake;

  // Indoor readout: repaint only when a fresh sensor sample lands (every 5 s).
  if (millis() - g_last_sensor_poll_ms >= SENSOR_POLL_MS) {
    g_last_sensor_poll_ms = millis();
    pollIndoor();
    if (g_booted) drawTemperatureConsole(night);
  }

  // Battery is in the top console; repaint it only when the percentage moves.
  bool solar_rose = false;
  if (g_battery_pct < 0 ||
      millis() - g_last_battery_poll_ms >= BATTERY_POLL_MS) {
    g_last_battery_poll_ms = millis();
    int prev_pct = g_battery_pct;
    pollBattery();
    if (g_booted && g_battery_pct != prev_pct) drawTimeConsole(night);
    // Battery climbing on its own (no USB enumerated) = charging from the sun.
    if (prev_pct >= 0 && g_battery_pct >= prev_pct + 2 && !g_usb_seen) solar_rose = true;
  }

  // On the day<->night boundary, play the wind-down / wake-up animation, then
  // repaint everything in the new palette. Skip the animation on the first
  // classification after boot (nothing to transition from).
  static int s_last_awake = -1;
  if (g_booted && (int)awake != s_last_awake) {
    bool first = (s_last_awake < 0);
    s_last_awake = awake;
    if (!first) {
      if (awake) playWakeTransition();
      else       playSleepTransition();
      g_static_drawn = false;  // the animation overwrote the whole screen
    }
    renderAll();
  }

  // The clock shows HH:MM (no seconds), so repaint the top console only when
  // the displayed minute changes.
  struct tm nowtm;
  static int s_last_min = -1;
  if (g_booted && g_ntp_configured && getLocalTime(&nowtm, 0) &&
      nowtm.tm_min != s_last_min) {
    s_last_min = nowtm.tm_min;
    drawTimeConsole(night);
  }

  // Science activities. Reactive triggers (weather change, USB power, solar
  // charge) take priority over the random idle timer; everything is suppressed
  // during quiet hours and held off by a 12-min floor so it can't spam.
  int pending = -1;
  static int  s_last_wcode    = -999;
  static bool s_usb_prev      = false;
  static uint32_t s_last_solar_ms   = 0;
  static uint32_t s_last_weather_ms = 0;
  static bool s_solar_fired   = false;
  static bool s_weather_fired = false;
  if (g_weather.valid && g_weather.weather_code != s_last_wcode) {
    if (s_last_wcode != -999 &&
        (!s_weather_fired || millis() - s_last_weather_ms >= WEATHER_COOLDOWN_MS))
      pending = ACT_WEATHER;                            // skip the first classification
    s_last_wcode = g_weather.weather_code;
  }
  if (g_usb_seen && !s_usb_prev) pending = ACT_REACTOR;  // external power just appeared
  s_usb_prev = g_usb_seen;
  if (solar_rose &&
      (!s_solar_fired || millis() - s_last_solar_ms >= SOLAR_COOLDOWN_MS))
    pending = ACT_SOLAR;                                 // battery climbing on the sun
  if (pending < 0 && millis() >= g_next_activity_ms) pending = pickIdleActivity();

  if (pending >= 0 && g_booted && awake &&
      millis() - g_last_activity_ms >= ACTIVITY_GAP_MIN_MS) {
    if (pending == ACT_SOLAR)        { s_last_solar_ms = millis();   s_solar_fired = true; }
    else if (pending == ACT_WEATHER) { s_last_weather_ms = millis(); s_weather_fired = true; }
    playActivity((uint8_t)pending);
    afterActivity();
    scheduleNextActivity();
  }

  // Rear LED breathes 0..ceiling in raw channel units (global brightness is
  // full, giving the LED's complete 8-bit range). The ceiling tracks the
  // backlight sun-curve, so the LED dims with the screen — lower ceiling when
  // the sun is low, brightest at midday — and the pulse fades fully off at the
  // bottom and during the rest between breaths.
  float led_curve = (float)targetBacklightDuty() / (float)BACKLIGHT_DUTY_PEAK;
  if (led_curve < 0.35f) led_curve = 0.35f;               // keep a visible night heartbeat
  uint8_t led_ceil = (uint8_t)(LED_CEIL * led_curve + 0.5f);
  // Gamma < 1 lifts the dim end of the breath so the fade rushes through the
  // lowest few levels (where 8-bit steps are most visible) instead of dwelling
  // there — the main cause of the leftover stepping near zero.
  float shaped = powf(ledBreath(), LED_GAMMA);
  uint8_t b = (uint8_t)(led_ceil * shaped + 0.5f);
  if      (g_battery_pct < 0  ) g_led[0] = CRGB(0, 0, b);       // unknown: blue
  else if (g_battery_pct < 20 ) g_led[0] = CRGB(b, 0, 0);       // low: red
  else if (g_battery_pct < 50 ) g_led[0] = CRGB(b, b/2, 0);     // mid: yellow
  else                           g_led[0] = CRGB(0, b, 0);       // good: green
  FastLED.show();

  delay(100);
}
