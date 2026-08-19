// Animations: sleep/wake night transitions + the connectivity trouble screen.
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

static void gfxWave(Adafruit_GFX& g, int cx, int cy, float dir, int r, uint16_t col) {
  int px = 0, py = 0;
  for (int i = 0; i <= 8; i++) {
    float ang = dir - 0.95f + i * (1.9f / 8.0f);
    int x = cx + (int)(cosf(ang) * r);
    int y = cy + (int)(sinf(ang) * r);
    if (i > 0) g.drawLine(px, py, x, y, col);
    px = x; py = y;
  }
}

// Non-linear progress 0..100: monotonic, but with a randomized wave so the
// percentage speeds up and slows down instead of ticking evenly. Reaches 0 and
// 100 exactly at the endpoints. Call progressInit() once per activity to
// re-roll the wave; then map elapsed-fraction (0..1) with progressPct().
static float     s_progA[3];
static float     s_progP[3];
static const int s_progN[3] = {2, 3, 5};
// ---------- sleep / wake transitions (night boundary) ----------

// Moon rises into place, a sleepy "Zzz" drifts up, then "GOODNIGHT" + city.
void playSleepTransition() {
  g_loading_bg_drawn = false;
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
  g_loading_bg_drawn = false;
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

// ---------- connectivity trouble screen ----------
static void gfxCloud(Adafruit_GFX& g, int cx, int cy, uint16_t col) {
  g.fillCircle(cx - 11, cy + 2, 9, col);
  g.fillCircle(cx + 11, cy + 2, 9, col);
  g.fillCircle(cx, cy - 6, 12, col);
  g.fillRect(cx - 20, cy + 2, 41, 10, col);
}
// Friendly "uh-oh, retrying" screen for first-boot connectivity failures: a
// themed icon searching in vain, a blinking "?", and an animated RETRYING — so
// a hiccup feels like the lander trying, not an error.
// theme: 0 = wifi, 1 = location, 2 = weather.
void playTrouble(const char* msg, int theme, uint32_t duration_ms) {
  GFXcanvas16 cv(240, 240);
  g_loading_bg_drawn = false;
  uint32_t t0 = millis();
  while (millis() - t0 < duration_ms) {
    uint32_t e = millis() - t0;
    int cx = 120, cy = 110 + (int)(3 * sinf(e / 450.0f));    // slow gentle bob
    cv.fillScreen(ST77XX_BLACK); gfxStars(cv);
    if (theme == 0) {                                        // wifi: pings fade into the void
      cv.drawLine(cx, cy + 28, cx, cy + 8, 0xAD55);          // mast
      cv.fillTriangle(cx - 11, cy + 8, cx + 11, cy + 8, cx, cy + 19, 0xBDF7);  // dish
      for (int k = 0; k < 3; k++) {
        int r = 6 + (int)((e / 15 + k * 26) % 78);
        gfxWave(cv, cx, cy + 6, -1.5708f, r, r > 52 ? 0x2124 : 0x07FF);
      }
    } else if (theme == 1) {                                 // location: a wobbling globe
      cv.fillCircle(cx, cy, 22, 0x0D5F);
      cv.fillCircle(cx - 8, cy - 5, 6, 0x2E6A);
      cv.fillCircle(cx + 8, cy + 6, 5, 0x2E6A);
      int w = (int)(cosf(e / 650.0f) * 22);
      cv.drawLine(cx - w, cy, cx + w, cy, 0x6BFD);
    } else {                                                 // weather: glitchy cloud
      gfxCloud(cv, cx, cy, 0xAD75);
      for (int d = 0; d < 7; d++)
        cv.drawPixel(cx - 22 + (int)random(45), cy - 9 + (int)random(22), ST77XX_WHITE);
    }
    if ((e / 950) % 2) gfxCenterText(cv, "?", 44, 3, ST77XX_YELLOW);   // slow blinking ?
    gfxCenterText(cv, msg, 170, 2, COLOR_ORANGE);
    gfxCenterText(cv, "RETRYING", 200, 1, NIGHT_TEXT);
    tft.drawRGBBitmap(0, 0, cv.getBuffer(), 240, 240);
    delay(40);
  }
}
