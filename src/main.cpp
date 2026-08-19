// Lander firmware.
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
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>
#include <Adafruit_SHT31.h>

#include "lander_logic.h"

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

constexpr uint32_t SENSOR_POLL_MS  = 5000UL;
constexpr uint32_t BATTERY_POLL_MS = 30000UL;

// TFT_eSPI compiled cleanly but drew nothing at all on this bare module, so the
// panel is driven with Adafruit_ST7789 + Adafruit_GFX instead.
Adafruit_ST7789 tft(PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST);
Adafruit_SHT31  sensor;

struct IndoorReading {
  float temperature_f = 0.0f;
  float humidity_pct  = 0.0f;
  bool  valid         = false;
};

IndoorReading g_indoor;
bool g_sensor_ok   = false;
int  g_battery_pct = -1;  // -1 until first read

static uint32_t g_last_sensor_poll_ms  = 0;
static uint32_t g_last_battery_poll_ms = 0;

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
  tft.fillRect(10, 90, 220, 70, ST77XX_BLACK);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(2);
  tft.setCursor(20, 96);
  if (g_indoor.valid) tft.printf("%.2fF %.2f%%", g_indoor.temperature_f, g_indoor.humidity_pct);
  else                tft.print("no sensor");
  tft.setCursor(20, 130);
  if (g_battery_pct >= 0) tft.printf("batt %d%%", g_battery_pct);
  else                    tft.print("batt --");
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
