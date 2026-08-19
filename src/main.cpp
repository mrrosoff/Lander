// Lander firmware: WiFi -> ipinfo.io (geolocation) -> Open-Meteo (weather +
// sunrise/sunset + tz) -> NTP for the clock.
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
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <Adafruit_SHT31.h>
#include <time.h>

#include "secrets.h"
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
constexpr uint8_t  BACKLIGHT_DUTY        = 28;  // ~11% of 255

constexpr uint32_t WIFI_TIMEOUT_MS    = 30000;
constexpr uint32_t WEATHER_REFRESH_MS = 30UL * 60UL * 1000UL;
constexpr uint32_t SENSOR_POLL_MS     = 5000UL;
constexpr uint32_t BATTERY_POLL_MS    = 30000UL;
constexpr uint32_t RETRY_BACKOFF_MS   = 30UL * 1000UL;
constexpr uint32_t NTP_RETRY_MS       = 15UL * 1000UL;  // re-sync clock while stuck on SYNCING

// TFT_eSPI compiled cleanly but drew nothing at all on this bare module, so the
// panel is driven with Adafruit_ST7789 + Adafruit_GFX instead.
Adafruit_ST7789 tft(PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST);
Adafruit_SHT31  sensor;

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

Location      g_location;
Weather       g_weather;
IndoorReading g_indoor;
bool g_ntp_configured = false;
bool g_sensor_ok   = false;
int  g_battery_pct = -1;  // -1 until first read

static uint32_t g_last_weather_fetch_ms = 0;
static uint32_t g_last_sensor_poll_ms   = 0;
static uint32_t g_last_battery_poll_ms  = 0;
static uint32_t g_last_ntp_retry_ms     = 0;

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

// ---------- WiFi / data fetch ----------
static bool connectWifi() {
  WiFi.mode(WIFI_STA);
  WiFi.setScanMethod(WIFI_ALL_CHANNEL_SCAN);
  WiFi.setTxPower(WIFI_POWER_8_5dBm);

  int found = WiFi.scanNetworks();
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
  if (!best) return false;

  WiFi.begin(best->ssid, best->pass);
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_TIMEOUT_MS) delay(50);
  if (WiFi.status() != WL_CONNECTED) return false;
  Serial.printf("WiFi: %s RSSI=%d\n", WiFi.localIP().toString().c_str(), WiFi.RSSI());
  return true;
}

static bool fetchLocation() {
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
  return g_location.valid;
}

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

static bool fetchWeather() {
  if (!g_location.valid) return false;

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
  return g_weather.valid;
}

static void wifiOff() {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
}

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

static void drawReadings() {
  tft.fillRect(0, 70, 240, 130, ST77XX_BLACK);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(2);

  struct tm now;
  tft.setCursor(20, 80);
  if (g_ntp_configured && getLocalTime(&now, 0)) tft.printf("%02d:%02d", now.tm_hour, now.tm_min);
  else                                           tft.print("--:--");

  tft.setCursor(20, 110);
  if (g_location.valid) tft.print(g_location.city);

  tft.setCursor(20, 140);
  if (g_weather.valid) tft.printf("out %.1fF wmo%d", g_weather.temperature_f, g_weather.weather_code);

  tft.setCursor(20, 170);
  if (g_indoor.valid) tft.printf("in %.2fF %.2f%%", g_indoor.temperature_f, g_indoor.humidity_pct);

  tft.setCursor(20, 200);
  if (g_battery_pct >= 0) tft.printf("batt %d%%", g_battery_pct);
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("Lander boot");

  ledcSetup(BACKLIGHT_PWM_CHANNEL, BACKLIGHT_PWM_FREQ_HZ, BACKLIGHT_PWM_BITS);
  ledcAttachPin(PIN_BACKLIGHT, BACKLIGHT_PWM_CHANNEL);
  ledcWrite(BACKLIGHT_PWM_CHANNEL, BACKLIGHT_DUTY);

  SPI.begin(PIN_TFT_SCLK, -1, PIN_TFT_MOSI, PIN_TFT_CS);
  tft.init(240, 240, SPI_MODE0);
  tft.setSPISpeed(20000000);
  tft.setRotation(2);
  tft.setTextWrap(false);

  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL);
  g_sensor_ok = sensor.begin(0x44);
  if (!g_sensor_ok) Serial.println("SHT31 init FAILED");

  tft.fillScreen(ST77XX_BLACK);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(3);
  tft.setCursor(48, 40);
  tft.print("LANDER");
}

void loop() {
  // Refresh weather on a slow cadence and keep the radio off in between. The
  // first fetch is always allowed so there is data to show.
  bool need_weather = !g_weather.valid ||
      (millis() - g_last_weather_fetch_ms) >= WEATHER_REFRESH_MS;

  if (need_weather) {
    if (!connectWifi()) { delay(RETRY_BACKOFF_MS); return; }
    if (!g_location.valid && !fetchLocation()) {
      wifiOff();
      delay(RETRY_BACKOFF_MS);
      return;
    }
    if (fetchWeather()) g_last_weather_fetch_ms = millis();
    else {
      wifiOff();
      delay(RETRY_BACKOFF_MS);
      return;
    }
    wifiOff();
    drawReadings();
  }

  // If weather came back but the clock never did, retry NTP on its own tight
  // timer instead of riding the 30-min weather cadence (WiFi is off between
  // refreshes, so SNTP can't recover on its own).
  if (!g_ntp_configured && g_weather.valid &&
      (millis() - g_last_ntp_retry_ms) >= NTP_RETRY_MS) {
    g_last_ntp_retry_ms = millis();
    if (connectWifi()) {
      syncNtp();
      wifiOff();
    }
  }

  if (g_battery_pct < 0 || millis() - g_last_battery_poll_ms >= BATTERY_POLL_MS) {
    g_last_battery_poll_ms = millis();
    pollBattery();
  }
  if (millis() - g_last_sensor_poll_ms >= SENSOR_POLL_MS) {
    g_last_sensor_poll_ms = millis();
    pollIndoor();
    drawReadings();
  }
  delay(100);
}
