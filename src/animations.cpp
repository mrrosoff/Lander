// Animations: sleep/wake night transitions.
//
// Every frame is composited into an offscreen GFXcanvas16 and pushed in a
// single blit, so the panel never shows an intermediate blank state (no
// flashing).
#include <Arduino.h>
#include "lander.h"

// ---------- canvas draw helpers ----------
static void gfxStars(Adafruit_GFX& g) {
  for (uint8_t i = 0; i < N_STARS; i++)
    g.drawPixel(STARFIELD[i].x, STARFIELD[i].y, ST77XX_WHITE);
}

static void gfxMoon(Adafruit_GFX& g, int16_t cx, int16_t cy, int16_t r, uint16_t col) {
  g.fillCircle(cx, cy, r, col);
  g.fillCircle(cx - r / 3, cy - r / 5, r, ST77XX_BLACK);  // carve the crescent
}

static void gfxSun(Adafruit_GFX& g, int16_t cx, int16_t cy, int16_t r, uint16_t col) {
  for (int i = 0; i < 8; i++) {
    float a = i * 0.7854f;
    g.drawLine(cx + (int16_t)(cosf(a) * (r + 4)), cy + (int16_t)(sinf(a) * (r + 4)),
               cx + (int16_t)(cosf(a) * (r + 9)), cy + (int16_t)(sinf(a) * (r + 9)), col);
  }
  g.fillCircle(cx, cy, r, col);
}

static void gfxCenterText(Adafruit_GFX& g, const char* s, int16_t y, uint8_t size, uint16_t col) {
  g.setFont(NULL);
  g.setTextWrap(false);
  g.setTextSize(size);
  g.setTextColor(col);
  g.setCursor((240 - (int16_t)strlen(s) * 6 * size) / 2, y);
  g.print(s);
}

// ---------- sleep / wake transitions (night boundary) ----------

// Moon rises into place, a sleepy "Zzz" drifts up, then "GOODNIGHT" + city.
void playSleepTransition() {
  GFXcanvas16 cv(240, 240);
  String city = g_location.valid ? g_location.city : String("LANDER");
  city.toUpperCase();
  for (int step = 0; step <= 18; step++) {
    int16_t my = 250 - (250 - 92) * min(step, 16) / 16;
    cv.fillScreen(ST77XX_BLACK);
    gfxStars(cv);
    gfxMoon(cv, 120, my, 26, NIGHT_MOON);
    if (step >= 9) {
      cv.setFont(NULL); cv.setTextWrap(false); cv.setTextColor(NIGHT_GREEN);
      cv.setTextSize(2); cv.setCursor(158, my - 24); cv.print("z");
    }
    if (step >= 12) { cv.setTextSize(3); cv.setCursor(176, my - 48); cv.print("Z"); }
    if (step >= 16) {  // settle: title appears once the moon is home
      gfxCenterText(cv, "GOODNIGHT", 150, 2, NIGHT_MAGENTA);
      gfxCenterText(cv, city.c_str(), 175, 1, NIGHT_TEXT);
    }
    tft.drawRGBBitmap(0, 0, cv.getBuffer(), 240, 240);
    delay(step >= 16 ? 600 : 40);
  }
}

// Sun rises as the moon sinks away, then "GOOD MORNING".
void playWakeTransition() {
  GFXcanvas16 cv(240, 240);
  for (int step = 0; step <= 18; step++) {
    int16_t sy    = 250 - (250 - 120) * min(step, 16) / 16;
    int16_t moonY = 55 + step * 5;
    cv.fillScreen(ST77XX_BLACK);
    gfxStars(cv);
    if (step < 9) gfxMoon(cv, 70, moonY, 18, 0x4208);  // dim moon sinks away
    gfxSun(cv, 120, sy, 18, ST77XX_YELLOW);
    if (step >= 16) gfxCenterText(cv, "GOOD MORNING", 170, 2, ST77XX_CYAN);
    tft.drawRGBBitmap(0, 0, cv.getBuffer(), 240, 240);
    delay(step >= 16 ? 600 : 40);
  }
}
