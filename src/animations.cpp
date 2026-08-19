// Animations: sleep/wake night transitions, the first-boot landing sequence,
// and the connectivity trouble screen.
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

static void gfxOrbit(Adafruit_GFX& g, int cx, int cy, int a, int b, float rot, uint16_t col) {
  float cr = cosf(rot), sr = sinf(rot);
  int px = 0, py = 0;
  for (int i = 0; i <= 24; i++) {
    float t = i * 0.2618f;  // 2*pi/24
    float ex = a * cosf(t), ey = b * sinf(t);
    int x = cx + (int)(ex * cr - ey * sr);
    int y = cy + (int)(ex * sr + ey * cr);
    if (i > 0) g.drawLine(px, py, x, y, col);
    px = x; py = y;
  }
}

// A short arc — a wifi-style wavefront of radius r centred on direction `dir`.
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

// ---------- first-boot landing sequence ----------
static void drawPlanet(GFXcanvas16& cv, int cx, int cy) {
  gfxOrbit(cv, cx, cy, 28, 9, 0.35f, 0x8C9A);    // ring, back half
  cv.fillCircle(cx, cy, 15, 0x3192);
  cv.fillCircle(cx - 4, cy - 3, 12, 0x6DBE);     // lit limb
  cv.fillCircle(cx + 6, cy + 5, 5, 0x2150);      // surface mottle
  gfxOrbit(cv, cx, cy, 28, 9, 0.35f, 0xCE59);    // ring, front half
}

static void drawSurface(GFXcanvas16& cv, int gy) {
  const uint16_t crust = 0xF52A, mid = 0xB2A4, deep = 0x6981, shadow = 0x4861;
  for (int x = 0; x < 240; x++) {
    int sy = gy + (int)(sinf(x * 0.04f) * 7.0f + sinf(x * 0.13f + 2.0f) * 3.0f);
    cv.drawFastVLine(x, sy, 240 - sy, deep);
    cv.drawFastVLine(x, sy, 9, mid);
    cv.drawFastVLine(x, sy, 3, crust);
  }
  const int cxs[3] = {68, 150, 198};
  const int cys[3] = {gy + 22, gy + 15, gy + 27};
  const int crs[3] = {12, 9, 7};
  for (int i = 0; i < 3; i++) {
    cv.fillCircle(cxs[i], cys[i], crs[i], shadow);
    cv.drawCircle(cxs[i], cys[i], crs[i], crust);
  }
}

static void drawLander(GFXcanvas16& cv, int cx, int cy) {
  const uint16_t body = 0xC618, dark = 0x8410, leg = 0xAD55;
  cv.drawLine(cx - 11, cy + 7, cx - 19, cy + 18, leg);   // legs
  cv.drawLine(cx + 11, cy + 7, cx + 19, cy + 18, leg);
  cv.fillRect(cx - 22, cy + 18, 7, 2, leg);              // feet
  cv.fillRect(cx + 15, cy + 18, 7, 2, leg);
  cv.fillTriangle(cx - 13, cy - 7, cx + 13, cy - 7, cx, cy - 20, dark);  // cone
  cv.fillRect(cx - 14, cy - 7, 28, 15, body);            // body
  cv.drawRect(cx - 14, cy - 7, 28, 15, dark);
  cv.fillCircle(cx, cy, 5, ST77XX_CYAN);                 // window
  cv.fillCircle(cx - 1, cy - 1, 2, ST77XX_WHITE);
}

static void drawRetroFlame(GFXcanvas16& cv, int cx, int topY, int len, uint8_t f) {
  if (len <= 0) return;
  const uint16_t outer = (f & 1) ? ST77XX_YELLOW : COLOR_ORANGE;
  cv.fillTriangle(cx - 7, topY, cx + 7, topY, cx, topY + len, outer);
  cv.fillTriangle(cx - 3, topY, cx + 3, topY, cx, topY + (len * 2) / 3, ST77XX_WHITE);
}

void playLanding() {
  g_loading_bg_drawn = false;
  GFXcanvas16 cv(240, 240);
  const int gy = 196, landY = gy - 22, startY = 24, N = 64;

  for (int s = 0; s <= N; s++) {
    float f    = (float)s / N;
    float ease = 1.0f - (1.0f - f) * (1.0f - f);
    int   cy   = startY + (int)((landY - startY) * ease + 0.5f);
    cv.fillScreen(ST77XX_BLACK);
    gfxStars(cv);
    drawPlanet(cv, 46, 80);
    drawSurface(cv, gy);
    drawRetroFlame(cv, 120, cy + 8, 9 + (int)((1.0f - f) * 16), (uint8_t)s);
    drawLander(cv, 120, cy);
    tft.drawRGBBitmap(0, 0, cv.getBuffer(), 240, 240);
    delay(42);
  }

  const int ND = 22;
  float px[ND], py[ND], pvx[ND], pvy[ND];
  for (int i = 0; i < ND; i++) {
    float side = (i & 1) ? 1.0f : -1.0f;
    px[i]  = 120 + side * 15 + (random(9) - 4);
    py[i]  = gy - 2;
    pvx[i] = side * (0.7f + random(130) / 100.0f);
    pvy[i] = -(0.6f + random(150) / 100.0f);
  }
  const uint16_t dcol[3] = {0xF52A, 0xB2A4, 0x6981};
  for (int fr = 0; fr < 22; fr++) {
    uint16_t c = dcol[fr < 8 ? 0 : fr < 15 ? 1 : 2];
    cv.fillScreen(ST77XX_BLACK);
    gfxStars(cv);
    drawPlanet(cv, 46, 80);
    drawSurface(cv, gy);
    if (fr < 16) {
      cv.drawCircle(108, gy, 5 + fr * 3, c);
      cv.drawCircle(132, gy, 5 + fr * 3, c);
    }
    drawLander(cv, 120, landY);
    gfxCenterText(cv, "TOUCHDOWN", 30, 2, COLOR_ORANGE);
    for (int i = 0; i < ND; i++) {
      px[i] += pvx[i];
      py[i] += pvy[i];
      pvy[i] += 0.16f;                              // gravity
      pvx[i] *= 0.96f;                              // drag
      if (py[i] > gy) { py[i] = gy; pvx[i] *= 0.6f; }
      cv.fillCircle((int)px[i], (int)py[i], fr < 11 ? 2 : 1, c);
    }
    tft.drawRGBBitmap(0, 0, cv.getBuffer(), 240, 240);
    delay(48);
  }
  delay(700);

  for (int fr = 0; fr <= 26; fr++) {
    cv.fillScreen(ST77XX_BLACK);
    gfxStars(cv);
    drawPlanet(cv, 46, 80);
    drawSurface(cv, gy);
    drawLander(cv, 120, landY);
    int topY = landY - 20;
    cv.drawLine(120, topY, 120, topY - 9, 0xAD55);
    bool beacon = (fr / 2) & 1;
    cv.fillCircle(120, topY - 10, 2, beacon ? ST77XX_RED : 0x3000);
    if (beacon)
      for (int k = 1; k <= 3; k++)
        gfxWave(cv, 120, topY - 10, -1.5708f, 4 + k * 6, 0x07FF);
    cv.fillCircle(112, landY + 1, 1, (fr % 3 == 0) ? ST77XX_GREEN  : 0x0320);
    cv.fillCircle(128, landY + 1, 1, (fr % 3 == 1) ? ST77XX_YELLOW : 0x3200);
    gfxCenterText(cv, "INITIALIZING", 30, 2, ST77XX_CYAN);
    tft.drawRGBBitmap(0, 0, cv.getBuffer(), 240, 240);
    delay(70);
  }
  delay(500);

  for (int y = 240; y >= 0; y -= 18) {
    cv.fillRect(0, y, 240, 240 - y, ST77XX_BLACK);
    tft.drawRGBBitmap(0, 0, cv.getBuffer(), 240, 240);
    delay(18);
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
