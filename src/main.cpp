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

#include <Arduino.h>
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7789.h>

constexpr int8_t  PIN_TFT_CS    = 11;
constexpr int8_t  PIN_TFT_DC    = 10;
constexpr int8_t  PIN_TFT_RST   = 5;
constexpr uint8_t PIN_BACKLIGHT = 12;
constexpr int8_t  PIN_TFT_MOSI  = 6;
constexpr int8_t  PIN_TFT_SCLK  = 7;

constexpr uint8_t  BACKLIGHT_PWM_CHANNEL = 0;
constexpr uint32_t BACKLIGHT_PWM_FREQ_HZ = 5000;
constexpr uint8_t  BACKLIGHT_PWM_BITS    = 8;
constexpr uint8_t  BACKLIGHT_DUTY        = 28;  // ~11% of 255

// TFT_eSPI compiled cleanly but drew nothing at all on this bare module, so the
// panel is driven with Adafruit_ST7789 + Adafruit_GFX instead.
Adafruit_ST7789 tft(PIN_TFT_CS, PIN_TFT_DC, PIN_TFT_RST);

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

  tft.fillScreen(ST77XX_BLACK);
  tft.drawRect(0, 0, 240, 240, ST77XX_WHITE);
  tft.setTextColor(ST77XX_WHITE);
  tft.setTextSize(3);
  tft.setCursor(48, 110);
  tft.print("LANDER");
}

void loop() {
  // Heartbeat until there is something real to show.
  static bool on = false;
  on = !on;
  tft.fillCircle(120, 170, 4, on ? ST77XX_WHITE : ST77XX_BLACK);
  delay(500);
}
