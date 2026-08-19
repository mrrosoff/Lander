// Display: the clock/console UI (bhoite's Boron Lander layout) and the
// WMO weather-code to icon mapping.
//
// Top zone (time/city/icon) and bottom strip (temp/humidity) render into
// offscreen canvases and blit; the middle band (sun times / NASA / dividers)
// is drawn straight to the panel once.
#include <Arduino.h>
#include <time.h>
#include "lander.h"

#include "weather_icons.h"
#include "fonts/Orbitron_Medium_40.h"
#include "fonts/Orbitron_Bold_20.h"
#include <Fonts/FreeSansOblique9pt7b.h>
#include "fonts/Roboto_Condensed_Light_Italic_24.h"

// Offscreen buffers (bhoite's pattern). Top is the time/city/icon zone, bottom
// is the temp/humidity strip.
static GFXcanvas16 timeCanvas(240, 110);
static GFXcanvas16 tempCanvas(240, 40);

// ---------- formatting ----------
static String formatDate(const struct tm& t) {
  static const char* days[]   = {"SUN","MON","TUE","WED","THU","FRI","SAT"};
  static const char* months[] = {"JAN","FEB","MAR","APR","MAY","JUN",
                                  "JUL","AUG","SEP","OCT","NOV","DEC"};
  char buf[24];
  snprintf(buf, sizeof(buf), "%s,%s %d,%d",
           days[t.tm_wday], months[t.tm_mon], t.tm_mday, t.tm_year + 1900);
  return String(buf);
}

static void format12h(const struct tm& t, char* hhmm, size_t n, const char*& ampm) {
  int h = t.tm_hour % 12; if (h == 0) h = 12;
  snprintf(hhmm, n, "%d:%02d", h, t.tm_min);
  ampm = t.tm_hour < 12 ? " AM" : " PM";
}

// ---------- WMO weather code -> icon bitmap ----------
const uint16_t* iconForWeatherCode(int code) {
  if (code == 0)                              return sunny;
  if (code == 1 || code == 2)                 return partial_cloudy;
  if (code == 3)                              return cloudy;
  if (code == 45 || code == 48)               return cloud;
  if (code >= 51 && code <= 57)               return rain;
  if (code >= 61 && code <= 67)               return rain;
  if (code >= 71 && code <= 77)               return snow;
  if (code >= 80 && code <= 82)               return rain;
  if (code == 85 || code == 86)               return snow;
  if (code == 95)                             return thunder;
  if (code == 96 || code == 99)               return rain_thunder;
  return cloud;
}

// ---------- static middle band (drawn once) ----------
void drawStaticMiddle() {
  tft.fillScreen(ST77XX_BLACK);
  tft.drawRGBBitmap(15,  135, sunrise, 40, 31);
  tft.drawRGBBitmap(85,  135, sunset,  40, 31);
  tft.drawRGBBitmap(160, 128, nasa,    80, 66);
  tft.drawLine(0, 120, 240, 120, COLOR_DIVIDER);
  tft.drawLine(0, 199, 240, 199, COLOR_DIVIDER);
  g_static_drawn = true;
}

// ---------- top canvas: date, battery, time, AM/PM, city, weather icon ----------
void drawTimeConsole() {
  const uint16_t cDate = ST77XX_CYAN;
  const uint16_t cTime = ST77XX_GREEN;
  const uint16_t cAmpm = COLOR_ORANGE;
  const uint16_t cCity = COLOR_PINK;
  const uint16_t cBatt = ST77XX_WHITE;

  timeCanvas.fillScreen(ST77XX_BLACK);

  struct tm now;
  bool have_time = g_ntp_configured && getLocalTime(&now, 0);

  // Cache last known time so we never show "--:--" after first sync
  static struct tm s_last_tm;
  static bool s_have_last_tm = false;
  if (have_time) {
    s_last_tm = now;
    s_have_last_tm = true;
  } else if (s_have_last_tm) {
    now = s_last_tm;
    have_time = true;
  }

  // Date (Roboto Italic 24, baseline at y=30)
  timeCanvas.setFont(&Roboto_Condensed_Light_Italic_24);
  timeCanvas.setTextSize(1);
  timeCanvas.setTextColor(cDate);
  timeCanvas.setCursor(0, 30);
  timeCanvas.print(have_time ? formatDate(now) : String("SYNCING..."));

  // Time HH:MM (Orbitron Medium 40, baseline at y=75)
  char hhmm[8]; const char* ampm = "";
  if (have_time) format12h(now, hhmm, sizeof(hhmm), ampm);
  else { strcpy(hhmm, "--:--"); ampm = " --"; }

  timeCanvas.setFont(&Orbitron_Medium_40);
  timeCanvas.setTextColor(cTime);
  timeCanvas.setCursor(0, 75);
  timeCanvas.print(hhmm);

  // AM/PM (Orbitron Bold 20) right after the time
  int16_t bx, by; uint16_t bw, bh;
  timeCanvas.getTextBounds(hhmm, 0, 75, &bx, &by, &bw, &bh);
  timeCanvas.setFont(&Orbitron_Bold_20);
  timeCanvas.setTextColor(cAmpm);
  timeCanvas.setCursor(bw, 60);
  timeCanvas.print(ampm);

  // City (Roboto Italic 24, baseline at y=109 — top of canvas region)
  String city = g_location.valid ? g_location.city : String("LANDER");
  city.toUpperCase();
  timeCanvas.setFont(&Roboto_Condensed_Light_Italic_24);
  timeCanvas.setTextColor(cCity);
  timeCanvas.setCursor(0, 109);
  timeCanvas.print(city);

  // Battery / USB indicator — top-right corner, icon + text on one line (y: 4-12)
  timeCanvas.setFont(NULL); timeCanvas.setTextSize(1); timeCanvas.setTextWrap(false);
  if (g_usb_seen) {
    timeCanvas.setTextColor(ST77XX_CYAN);
    timeCanvas.setCursor(220, 4);
    timeCanvas.print("USB");
  } else {
    // Battery body 20×8 + nub 2×4, top of canvas
    timeCanvas.drawRect(188, 4, 20, 8, COLOR_BATT);
    timeCanvas.fillRect(208, 6, 2, 4, COLOR_BATT);
    if (g_battery_pct >= 0) {
      int fill_w = 16 * g_battery_pct / 100;
      uint16_t fc = g_battery_pct < 20 ? ST77XX_RED :
                    g_battery_pct < 50 ? ST77XX_YELLOW : ST77XX_GREEN;
      if (fill_w > 0) timeCanvas.fillRect(190, 6, fill_w, 4, fc);
    }
    char batbuf[8];
    if (g_battery_pct >= 0) snprintf(batbuf, sizeof(batbuf), "%d%%", g_battery_pct);
    else                    snprintf(batbuf, sizeof(batbuf), "-%");
    timeCanvas.setTextColor(cBatt);
    timeCanvas.setCursor(216, 4);
    timeCanvas.print(batbuf);
  }

  if (g_weather.valid) {
    // Weather icon top-right: 40×40 native
    timeCanvas.drawRGBBitmap(192, 35, iconForWeatherCode(g_weather.weather_code), 40, 40);
    char outbuf[8];
    snprintf(outbuf, sizeof(outbuf), "%.0f F", g_weather.temperature_f);
    timeCanvas.setFont(NULL);
    timeCanvas.setTextSize(2);
    timeCanvas.setTextColor(ST77XX_WHITE);
    int wpix = (int)strlen(outbuf) * 12;
    timeCanvas.setCursor(212 - wpix / 2, 80);
    timeCanvas.print(outbuf);
  }

  tft.drawRGBBitmap(0, 0, timeCanvas.getBuffer(), 240, 110);
}

// ---------- bottom canvas: temp | humidity (2 columns, 2 decimal places) ----------
void drawTemperatureConsole() {
  const uint16_t cText = ST77XX_WHITE;

  tempCanvas.fillScreen(ST77XX_BLACK);
  tempCanvas.setTextWrap(false);
  char buf[12];

  // Col 1: Mohit's thermometer bitmap (15×30, y=5..35, center≈20)
  tempCanvas.drawRGBBitmap(8, 5, thermometer, 15, 30);
  tempCanvas.setFont(&FreeSansOblique9pt7b);
  tempCanvas.setTextSize(1);
  tempCanvas.setTextColor(cText);
  if (g_indoor.valid) snprintf(buf, sizeof(buf), "%.2f", g_indoor.temperature_f);
  else                snprintf(buf, sizeof(buf), "--");
  tempCanvas.setCursor(30, 28);
  tempCanvas.print(buf);
  // Degree circle + F drawn after the number
  int16_t ncx = tempCanvas.getCursorX();
  tempCanvas.drawCircle(ncx + 3, 14, 2, cText);
  tempCanvas.setCursor(ncx + 7, 28);
  tempCanvas.print("F");

  // Col 2: drop icon centered in 40px bar (y=7..34)
  tempCanvas.fillTriangle(122, 7, 114, 26, 130, 26, 0x02FF);
  tempCanvas.fillCircle(122, 26, 8, 0x02FF);
  if (g_indoor.valid) snprintf(buf, sizeof(buf), "%.2f%%", g_indoor.humidity_pct);
  else                snprintf(buf, sizeof(buf), "--%");
  tempCanvas.setCursor(140, 28);
  tempCanvas.print(buf);

  tft.drawRGBBitmap(0, 200, tempCanvas.getBuffer(), 240, 40);
}

// ---------- middle-band sunrise/sunset times (default size 2) ----------
void drawSunTimes() {
  // Clear the strip between dividers under the icons (avoid touching icons).
  // Times sit to the right of the sun icons at (5,175); icons end at x=125.
  tft.fillRect(0, 165, 160, 25, ST77XX_BLACK);
  tft.setFont(NULL);
  tft.setTextSize(2);
  tft.setTextColor(ST77XX_YELLOW);
  tft.setCursor(5, 175);
  String s = g_weather.sunrise_hhmm + " " + g_weather.sunset_hhmm;
  tft.print(s);
}

void renderAll() {
  if (!g_static_drawn) drawStaticMiddle();
  drawTimeConsole();
  drawSunTimes();
  drawTemperatureConsole();
}
