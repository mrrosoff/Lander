// Animations: sleep/wake night transitions + science-activity takeovers.
//
// Every frame is composited into an offscreen GFXcanvas16 and pushed in a
// single blit, so the panel never shows an intermediate blank state (no
// flashing).
#include <Arduino.h>
#include "lander.h"
#include "lander_logic.h"

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

static void gfxProgress(Adafruit_GFX& g, int pct, uint16_t col) {
  g.drawRect(40, 220, 160, 12, COLOR_DIVIDER);
  int w = 156 * pct / 100;
  if (w > 0) g.fillRect(42, 222, w, 8, col);
}

// Tilted ellipse outline (an electron orbit), sampled as line segments.
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
static void progressInit() {
  for (int k = 0; k < 3; k++) {
    s_progA[k] = 0.12f + random(15) / 100.0f;   // amplitudes; sum < 1 keeps it monotonic
    s_progP[k] = random(628) / 100.0f;          // phase 0..2pi
  }
}
static int progressPct(float f) {
  if (f < 0) f = 0; else if (f > 1) f = 1;
  float g = f;
  for (int k = 0; k < 3; k++) {
    float w = 2 * 3.14159265f * s_progN[k];
    g += s_progA[k] / w * (sinf(w * f + s_progP[k]) - sinf(s_progP[k]));
  }
  if (g < 0) g = 0; else if (g > 1) g = 1;
  return (int)(g * 100 + 0.5f);
}

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

// ---------- science activities ----------
// Each runs ACTIVITY_RUN_MS of animation, then holds a result for ACTIVITY_HOLD_MS.

static const char* const ROCKS[] = {"BASALT", "OLIVINE", "QUARTZ",
                                     "REGOLITH", "ICE CORE", "HEMATITE"};

// EXCAVATING — subsurface scan sweeps the strata, locks onto a buried rock,
// then a tractor beam lifts the sample up to the collector bay.
static void actExcavate(GFXcanvas16& cv) {
  static const uint16_t GEMS[] = {0x07E0, 0xFA00, 0xFD20, 0x982F, 0x07FF, 0xF81F};
  const char* rock = ROCKS[random(6)];
  uint16_t gem = GEMS[random(6)];
  progressInit();
  const int rx0 = 50 + (int)random(145);  // buried target rock, random spot
  const int ry0 = 158 + (int)random(36);  // ...within the lower strata
  const int bayX = 120, bayY = 62;       // collector bay (top, padded below header)
  uint32_t t0 = millis();
  while (millis() - t0 < ACTIVITY_RUN_MS) {
    uint32_t e = millis() - t0;
    float p = (float)e / ACTIVITY_RUN_MS;
    cv.fillScreen(ST77XX_BLACK); gfxStars(cv);
    gfxCenterText(cv, "EXCAVATING", 24, 2, COLOR_ORANGE);
    // layered strata
    cv.fillRect(0, 124, 240, 30, 0x4208);
    cv.fillRect(0, 154, 240, 24, 0x5269);
    cv.fillRect(0, 178, 240, 24, 0x3186);
    cv.drawFastHLine(0, 124, 240, 0xAD55);             // surface
    // mineral flecks scattered through the strata
    cv.fillCircle(40, 166, 2, 0xFD20);  cv.fillCircle(96, 190, 2, 0x07FF);
    cv.fillCircle(176, 144, 2, 0x982F); cv.fillCircle(212, 192, 2, 0x07E0);
    cv.fillCircle(126, 158, 2, 0xF81F); cv.fillCircle(70, 146, 2, 0xFFE0);
    cv.fillCircle(58, 192, 5, 0x6B4D);                 // decoy rocks
    cv.fillCircle(204, 164, 4, 0x6B4D);
    // collector bay
    cv.fillRect(bayX - 16, bayY - 8, 32, 14, 0x4A8A);
    cv.drawRect(bayX - 16, bayY - 8, 32, 14, 0xBDF7);
    cv.fillTriangle(bayX - 10, bayY + 6, bayX + 10, bayY + 6, bayX, bayY + 14, 0x4A8A);
    if (p < 0.45f) {                                   // scanning sweep
      int sx = 30 + (int)((sinf(e / 360.0f) * 0.5f + 0.5f) * 180);
      cv.drawCircle(sx, ry0, 10, 0x07FF);
      cv.drawLine(sx - 15, ry0, sx - 6, ry0, 0x07FF);
      cv.drawLine(sx + 6, ry0, sx + 15, ry0, 0x07FF);
      cv.drawLine(sx, ry0 - 15, sx, ry0 - 6, 0x07FF);
      cv.drawLine(sx, ry0 + 6, sx, ry0 + 15, 0x07FF);
      cv.fillCircle(rx0, ry0, 7, gem);                 // buried sample
    } else {                                           // lock + tractor-beam lift
      float q = (p - 0.45f) / 0.45f; if (q > 1) q = 1;
      int rx = rx0 + (int)((bayX - rx0) * q);
      int ry = ry0 + (int)((bayY + 12 - ry0) * q);
      if ((e / 80) % 2) {                              // flickering beam cone
        cv.drawLine(bayX - 8, bayY + 8, rx - 7, ry, 0x07FF);
        cv.drawLine(bayX + 8, bayY + 8, rx + 7, ry, 0x05BF);
      }
      cv.drawCircle(rx0, ry0, 11, 0x05BF);             // lock ring at the dig site
      cv.fillCircle(rx, ry, 7, gem);                   // rising sample
      cv.fillCircle(rx - 2, ry - 2, 2, ST77XX_WHITE);  // glint
    }
    gfxProgress(cv, progressPct(p), COLOR_ORANGE);
    tft.drawRGBBitmap(0, 0, cv.getBuffer(), 240, 240);
    delay(35);
  }
  cv.fillScreen(ST77XX_BLACK); gfxStars(cv);
  gfxCenterText(cv, "EXCAVATING", 24, 2, COLOR_ORANGE);
  cv.fillCircle(120, 104, 18, gem);                    // recovered sample
  cv.fillCircle(112, 97, 6, ST77XX_WHITE);
  gfxProgress(cv, 100, ST77XX_GREEN);
  char buf[28]; snprintf(buf, sizeof(buf), "SAMPLE: %s", rock);
  gfxCenterText(cv, buf, 150, 2, ST77XX_GREEN);
  tft.drawRGBBitmap(0, 0, cv.getBuffer(), 240, 240);
  delay(ACTIVITY_HOLD_MS);
}

// Small vector weather glyphs for the analysis scan.
static void gfxCloud(Adafruit_GFX& g, int cx, int cy, uint16_t col) {
  g.fillCircle(cx - 11, cy + 2, 9, col);
  g.fillCircle(cx + 11, cy + 2, 9, col);
  g.fillCircle(cx, cy - 6, 12, col);
  g.fillRect(cx - 20, cy + 2, 41, 10, col);
}
static void drawWx(GFXcanvas16& g, int cx, int cy, int type) {
  switch (type) {
    case 0: gfxSun(g, cx, cy, 16, 0xFFE0); break;
    case 1: gfxSun(g, cx + 13, cy - 11, 9, 0xFFE0); gfxCloud(g, cx - 4, cy + 4, ST77XX_WHITE); break;
    case 2: gfxCloud(g, cx, cy, 0xAD75); break;
    case 3: gfxCloud(g, cx, cy - 6, 0x9CD3);
            for (int i = 0; i < 4; i++) g.drawLine(cx - 15 + i * 10, cy + 10, cx - 18 + i * 10, cy + 22, 0x05BF);
            break;
    case 4: gfxCloud(g, cx, cy - 6, 0xC618);
            for (int i = 0; i < 3; i++) { int sx = cx - 13 + i * 13, sy = cy + 16;
              g.drawLine(sx - 3, sy, sx + 3, sy, 0xBDFF); g.drawLine(sx, sy - 3, sx, sy + 3, 0xBDFF); }
            break;
    case 5: gfxCloud(g, cx, cy - 6, 0x9492);
            g.fillTriangle(cx + 1, cy + 6, cx - 6, cy + 18, cx + 2, cy + 15, 0xFFE0);
            g.fillTriangle(cx + 2, cy + 15, cx + 7, cy + 15, cx - 1, cy + 27, 0xFFE0);
            break;
  }
}

// ANALYZING WX — scans through the weather types, then locks on and reads out
// temperature, humidity, cloud cover and wind one field at a time.
static void actWeather(GFXcanvas16& cv) {
  static const char* NAMES[] = {"CLEAR", "PARTLY CLOUDY", "CLOUDY", "RAIN", "SNOW", "STORM"};
  int realType = g_weather.valid ? wxType(g_weather.weather_code) : 2;
  int finals[4] = {g_weather.valid ? (int)g_weather.temperature_f  : 60,
                   g_weather.valid ? (int)g_weather.humidity_pct    : 50,
                   g_weather.valid ? (int)g_weather.cloud_cover_pct : 40,
                   g_weather.valid ? (int)g_weather.wind_mph        : 6};
  const char* labels[4] = {"TEMP", "HUMIDITY", "CLOUD", "WIND"};
  const char* units[4]  = {"F", "%", "%", "MPH"};
  const int   lock[4]   = {2200, 4200, 6200, 8200};
  uint32_t t0 = millis();
  while (millis() - t0 < ACTIVITY_RUN_MS) {
    uint32_t e = millis() - t0;
    bool searching = (float)e / ACTIVITY_RUN_MS < 0.62f;
    cv.fillScreen(ST77XX_BLACK); gfxStars(cv);
    gfxCenterText(cv, "ANALYZING WX", 24, 2, ST77XX_CYAN);
    drawWx(cv, 120, 74, searching ? (int)((e / 280) % 6) : realType);
    gfxCenterText(cv, searching ? "SCANNING..." : NAMES[realType], 108, 2,
                  searching ? NIGHT_TEXT : ST77XX_WHITE);
    cv.drawFastHLine(20, 126, 200, COLOR_DIVIDER);
    cv.setFont(NULL); cv.setTextWrap(false); cv.setTextSize(2);
    for (int i = 0; i < 4; i++) {
      int y = 142 + i * 22;
      cv.setTextColor(0x07FF); cv.setCursor(20, y); cv.print(labels[i]);
      char vb[12];
      bool locked = (int)e >= lock[i];
      if (locked) snprintf(vb, sizeof(vb), "%d %s", finals[i], units[i]);
      else        snprintf(vb, sizeof(vb), "%02d %s", (int)((e / 80 + i * 37) % 100), units[i]);
      cv.setTextColor(locked ? ST77XX_GREEN : NIGHT_TEXT);
      cv.setCursor(220 - (int)strlen(vb) * 12, y); cv.print(vb);
    }
    tft.drawRGBBitmap(0, 0, cv.getBuffer(), 240, 240);
    delay(40);
  }
  cv.fillScreen(ST77XX_BLACK); gfxStars(cv);
  gfxCenterText(cv, "ANALYZING WX", 24, 2, ST77XX_CYAN);
  drawWx(cv, 120, 78, realType);
  gfxCenterText(cv, NAMES[realType], 112, 2, ST77XX_WHITE);
  char rb[26]; snprintf(rb, sizeof(rb), "%d F   %d%% RH", finals[0], finals[1]);
  gfxCenterText(cv, rb, 152, 2, ST77XX_GREEN);
  gfxCenterText(cv, "ANALYSIS COMPLETE", 186, 1, 0x07FF);
  tft.drawRGBBitmap(0, 0, cv.getBuffer(), 240, 240);
  delay(ACTIVITY_HOLD_MS);
}

// SOLAR ARRAY — sun beams photons into the panel, battery fills (solar charge).
static void actSolar(GFXcanvas16& cv) {
  int pct = (g_battery_pct >= 0) ? g_battery_pct : 50;
  uint32_t t0 = millis();
  while (millis() - t0 < ACTIVITY_RUN_MS) {
    uint32_t e = millis() - t0;
    cv.fillScreen(ST77XX_BLACK); gfxStars(cv);
    gfxCenterText(cv, "SOLAR ARRAY", 24, 2, ST77XX_YELLOW);
    gfxSun(cv, 60, 92, 22, ST77XX_YELLOW);             // static sun
    // 3D solar array: an iso parallelogram with a cell grid + front thickness
    cv.fillTriangle(95, 182, 200, 182, 230, 132, 0x2A94);
    cv.fillTriangle(95, 182, 230, 132, 125, 132, 0x2A94);
    cv.fillRect(95, 182, 106, 8, 0x114B);              // front-edge thickness
    for (int j = 1; j < 3; j++) {                      // grid rows
      int y = 182 - 50 * j / 3;
      cv.drawLine(95 + 30 * j / 3, y, 200 + 30 * j / 3, y, 0x547A);
    }
    for (int i = 1; i < 4; i++)                         // grid columns
      cv.drawLine(95 + 105 * i / 4, 182, 125 + 105 * i / 4, 132, 0x547A);
    cv.drawLine(95, 182, 200, 182, 0xBDF7);            // bright outline
    cv.drawLine(200, 182, 230, 132, 0xBDF7);
    cv.drawLine(230, 132, 125, 132, 0xBDF7);
    cv.drawLine(125, 132, 95, 182, 0xBDF7);
    cv.drawFastVLine(150, 190, 9, 0x8410);             // support post + foot
    cv.drawFastHLine(142, 199, 17, 0x8410);
    for (int i = 0; i < 5; i++) {                      // photons streaming to the array
      int prog = (int)(e / 11 + i * 28) % 120;
      cv.fillCircle(82 + prog, 100 + prog * 42 / 100, 2, ST77XX_WHITE);
    }
    cv.drawRect(60, 214, 120, 20, COLOR_BATT); cv.fillRect(180, 219, 4, 10, COLOR_BATT);
    cv.fillRect(62, 216, 116 * pct / 100, 16, ST77XX_GREEN);
    tft.drawRGBBitmap(0, 0, cv.getBuffer(), 240, 240);
    delay(35);
  }
  cv.fillScreen(ST77XX_BLACK); gfxStars(cv);
  gfxCenterText(cv, "SOLAR ARRAY", 24, 2, ST77XX_YELLOW);
  gfxSun(cv, 120, 108, 26, ST77XX_YELLOW);
  char bf[22]; snprintf(bf, sizeof(bf), "SOAKING RAYS  %d%%", pct);
  gfxCenterText(cv, bf, 200, 1, ST77XX_WHITE);
  tft.drawRGBBitmap(0, 0, cv.getBuffer(), 240, 240);
  delay(ACTIVITY_HOLD_MS);
}

// FISSION CORE — a tiny reactor: containment vessel + control rods, the atom as
// the glowing core, and neutrons streaming out of it. Stands in for "running on
// external (USB) power."
static void actReactor(GFXcanvas16& cv) {
  int pct = (g_battery_pct >= 0) ? g_battery_pct : 50;
  const int cx = 120, cy = 120;
  uint32_t t0 = millis();
  while (millis() - t0 < ACTIVITY_RUN_MS) {
    uint32_t e = millis() - t0;
    cv.fillScreen(ST77XX_BLACK); gfxStars(cv);
    gfxCenterText(cv, "FISSION CORE", 24, 2, 0x07FF);
    // containment vessel
    cv.drawRoundRect(cx - 46, 66, 92, 104, 10, 0x8410);
    cv.drawRoundRect(cx - 43, 69, 86, 98, 8, 0x52AA);
    // control rods bobbing down from the lid
    for (int r = 0; r < 4; r++) {
      int rx = cx - 27 + r * 18;
      int rl = 24 + (int)(10 * sinf(e / 420.0f + r));
      cv.fillRect(rx - 2, 66, 4, rl, 0xC618);
      cv.fillRect(rx - 3, 61, 6, 5, 0xBDF7);
    }
    // glowing core + orbiting atom
    int glow = 9 + (int)(4 * sinf(e / 180.0f));
    cv.fillCircle(cx, cy, glow + 5, 0x4208);
    cv.fillCircle(cx, cy, glow, 0xFD20);              // hot core
    cv.fillCircle(cx, cy, glow / 2, ST77XX_WHITE);
    for (int o = 0; o < 3; o++) {
      float rot = o * 1.0472f;
      gfxOrbit(cv, cx, cy, 30, 12, rot, 0x07BF);
      float t = e / 500.0f + o * 2.094f;
      float ox = 30 * cosf(t), oy = 12 * sinf(t);
      cv.fillCircle(cx + (int)(ox * cosf(rot) - oy * sinf(rot)),
                    cy + (int)(ox * sinf(rot) + oy * cosf(rot)), 2, ST77XX_YELLOW);
    }
    // neutrons stream continuously outward, each on its own phase so none of
    // them ever snap back to the centre together
    for (int n = 0; n < 6; n++) {
      float np = fmodf(e / 950.0f + n / 6.0f, 1.0f);
      int   d  = (int)(np * 40);
      float na = n * 1.047f + 0.3f;                  // fixed direction per neutron
      cv.fillCircle(cx + (int)(cosf(na) * d), cy + (int)(sinf(na) * d), 2,
                    np > 0.8f ? 0x6B00 : 0xFFE0);    // dim as it fades out
    }
    cv.drawRect(60, 214, 120, 20, COLOR_BATT); cv.fillRect(180, 219, 4, 10, COLOR_BATT);
    cv.fillRect(62, 216, 116 * pct / 100, 16, ST77XX_GREEN);
    tft.drawRGBBitmap(0, 0, cv.getBuffer(), 240, 240);
    delay(35);
  }
  cv.fillScreen(ST77XX_BLACK); gfxStars(cv);
  gfxCenterText(cv, "FISSION CORE", 24, 2, 0x07FF);
  gfxCenterText(cv, "CORE ONLINE", 108, 2, ST77XX_GREEN);
  gfxCenterText(cv, "EXTERNAL POWER", 140, 1, 0x07FF);
  tft.drawRGBBitmap(0, 0, cv.getBuffer(), 240, 240);
  delay(ACTIVITY_HOLD_MS);
}

// TRANSMITTING — signal pulses bounce back and forth between dish and Earth.
static void actTransmit(GFXcanvas16& cv) {
  const int ax = 56, atip = 158;     // dish/source: mast tip
  const int ex = 186, ey = 100;      // Earth (big, low on the screen)
  progressInit();
  uint32_t t0 = millis();
  while (millis() - t0 < ACTIVITY_RUN_MS) {
    uint32_t e = millis() - t0;
    cv.fillScreen(ST77XX_BLACK); gfxStars(cv);
    gfxCenterText(cv, "TRANSMITTING", 24, 2, COLOR_PINK);
    cv.fillCircle(ex, ey, 24, 0x0D3F);                 // Earth
    cv.fillCircle(ex - 7, ey - 5, 8, 0x2E6A);
    cv.fillCircle(ex + 9, ey + 6, 6, 0x2E6A);
    cv.fillCircle(ex - 3, ey + 11, 5, 0x2E6A);
    cv.drawLine(ax, 188, ax, atip, 0xAD55);            // mast
    cv.fillTriangle(ax - 9, atip, ax + 9, atip, ax, atip + 10, 0xBDF7);  // dish
    // waves bounce between the dish and the Earth's NEAR EDGE (not its centre)
    float dx = (float)(ex - ax), dy = (float)(ey - atip);
    float len = sqrtf(dx * dx + dy * dy);
    int   eex = ex - (int)(24 * dx / len);             // point on Earth's rim
    int   eey = ey - (int)(24 * dy / len);
    cv.drawLine(ax, atip, eex, eey, 0x02B5);           // faint signal path
    float dirOut = atan2f(dy, dx);
    for (int k = 0; k < 3; k++) {
      float u   = fmodf(e / 2200.0f + (float)k / 3.0f, 1.0f);
      float tri = 1.0f - fabsf(2.0f * u - 1.0f);       // 0 -> 1 -> 0 (bounce)
      float dir = (u < 0.5f) ? dirOut : dirOut + 3.14159f;  // face travel direction
      int wx = ax + (int)((eex - ax) * tri);
      int wy = atip + (int)((eey - atip) * tri);
      for (int r = 4; r <= 12; r += 4) gfxWave(cv, wx, wy, dir, r, 0x07FF);
    }
    char bf[16]; snprintf(bf, sizeof(bf), "UPLINK %d%%", progressPct((float)e / ACTIVITY_RUN_MS));
    gfxCenterText(cv, bf, 214, 2, ST77XX_WHITE);
    tft.drawRGBBitmap(0, 0, cv.getBuffer(), 240, 240);
    delay(35);
  }
  cv.fillScreen(ST77XX_BLACK); gfxStars(cv);
  gfxCenterText(cv, "TRANSMITTING", 24, 2, COLOR_PINK);
  cv.fillCircle(ex, ey, 24, 0x0D3F);
  gfxCenterText(cv, "DATA SENT", 155, 3, ST77XX_GREEN);
  gfxCenterText(cv, "UPLINK COMPLETE", 190, 1, 0x07FF);
  tft.drawRGBBitmap(0, 0, cv.getBuffer(), 240, 240);
  delay(ACTIVITY_HOLD_MS);
}

// Real constellations: star positions (screen coords, ~centered) + the edges
// that connect them into the familiar figure. A random one is drawn each run.
struct Constellation {
  const char*     name;
  const uint8_t (*star)[2];
  uint8_t         nstar;
  const uint8_t (*edge)[2];
  uint8_t         nedge;
};
static const uint8_t orion_s[][2]   = {{90,68},{150,76},{108,118},{120,124},{132,130},{100,176},{152,178}};
static const uint8_t orion_e[][2]   = {{0,1},{0,2},{1,4},{2,3},{3,4},{2,5},{4,6}};
static const uint8_t dipper_s[][2]  = {{150,96},{150,132},{186,136},{182,100},{148,80},{118,74},{90,72}};
static const uint8_t dipper_e[][2]  = {{0,1},{1,2},{2,3},{3,0},{3,4},{4,5},{5,6}};
static const uint8_t cass_s[][2]    = {{70,92},{102,124},{132,98},{162,128},{192,102}};
static const uint8_t cass_e[][2]    = {{0,1},{1,2},{2,3},{3,4}};
static const uint8_t cygnus_s[][2]  = {{120,66},{120,112},{120,164},{78,108},{162,114}};
static const uint8_t cygnus_e[][2]  = {{0,1},{1,2},{3,1},{1,4}};
static const uint8_t leo_s[][2]     = {{86,152},{94,116},{112,90},{140,84},{184,150},{150,122}};
static const uint8_t leo_e[][2]     = {{0,1},{1,2},{2,3},{0,5},{5,4},{3,5}};
static const Constellation CONSTELLATIONS[] = {
  {"ORION",      orion_s,  7, orion_e,  7},
  {"URSA MAJOR", dipper_s, 7, dipper_e, 7},
  {"CASSIOPEIA", cass_s,   5, cass_e,   4},
  {"CYGNUS",     cygnus_s, 5, cygnus_e, 4},
  {"LEO",        leo_s,    6, leo_e,    6},
};

// STAR CATALOG — trace a real constellation edge by edge, then log it.
static void actStargaze(GFXcanvas16& cv) {
  const Constellation& C = CONSTELLATIONS[random(sizeof(CONSTELLATIONS) / sizeof(CONSTELLATIONS[0]))];
  uint32_t t0 = millis();
  while (millis() - t0 < ACTIVITY_RUN_MS) {
    uint32_t e = millis() - t0;
    cv.fillScreen(ST77XX_BLACK); gfxStars(cv);
    gfxCenterText(cv, "STAR CATALOG", 24, 2, NIGHT_MAGENTA);
    float p = (float)e / (ACTIVITY_RUN_MS - 1200);
    if (p > 1) p = 1;
    int shown = (int)(p * C.nedge);
    for (int i = 0; i < shown; i++)
      cv.drawLine(C.star[C.edge[i][0]][0], C.star[C.edge[i][0]][1],
                  C.star[C.edge[i][1]][0], C.star[C.edge[i][1]][1], 0x6BFD);
    for (int i = 0; i < C.nstar; i++)
      cv.fillCircle(C.star[i][0], C.star[i][1], 2, ST77XX_WHITE);
    tft.drawRGBBitmap(0, 0, cv.getBuffer(), 240, 240);
    delay(40);
  }
  cv.fillScreen(ST77XX_BLACK); gfxStars(cv);
  gfxCenterText(cv, "STAR CATALOG", 24, 2, NIGHT_MAGENTA);
  for (int i = 0; i < C.nedge; i++)
    cv.drawLine(C.star[C.edge[i][0]][0], C.star[C.edge[i][0]][1],
                C.star[C.edge[i][1]][0], C.star[C.edge[i][1]][1], 0x6BFD);
  for (int i = 0; i < C.nstar; i++) cv.fillCircle(C.star[i][0], C.star[i][1], 3, ST77XX_WHITE);
  char bf[28]; snprintf(bf, sizeof(bf), "LOGGED: %s", C.name);
  gfxCenterText(cv, bf, 205, 2, ST77XX_GREEN);
  tft.drawRGBBitmap(0, 0, cv.getBuffer(), 240, 240);
  delay(ACTIVITY_HOLD_MS);
}

// DIAGNOSTICS — systems check ticks green one by one, then "ALL NOMINAL".
static void actDiag(GFXcanvas16& cv) {
  const char* labels[5] = {"POWER", "COMMS", "THERMAL", "NAV", "STRUCT"};
  char vals[5][12];
  snprintf(vals[0], 12, "%d%%", g_battery_pct >= 0 ? g_battery_pct : 0);
  snprintf(vals[1], 12, "LINK OK");
  if (g_indoor.valid) snprintf(vals[2], 12, "%.0fF", g_indoor.temperature_f);
  else                snprintf(vals[2], 12, "OK");
  snprintf(vals[3], 12, "LOCKED");
  snprintf(vals[4], 12, "STABLE");
  bool slow[5];
  for (int i = 0; i < 5; i++) slow[i] = random(2);  // only some "load" before the tick
  uint32_t t0 = millis();
  while (millis() - t0 < ACTIVITY_RUN_MS) {
    uint32_t e = millis() - t0;
    cv.fillScreen(ST77XX_BLACK); gfxStars(cv);
    gfxCenterText(cv, "DIAGNOSTICS", 22, 2, ST77XX_CYAN);
    cv.setFont(NULL); cv.setTextWrap(false); cv.setTextSize(2);
    for (int i = 0; i < 5; i++) {
      int appear = i * 1800;
      if ((int)e < appear) break;                 // this check hasn't started yet
      int local = (int)e - appear;
      int y = 70 + i * 30;
      cv.setTextColor(ST77XX_WHITE); cv.setCursor(14, y); cv.print(labels[i]);
      if (slow[i] && local < 540) {               // some checks "load" first
        for (int d = 0; d < 3; d++) {             // three dots, highlight sweeps across
          int dx = 198 + d * 11;
          bool on = ((local / 130) % 3) == d;
          cv.fillCircle(dx, y + 7, on ? 3 : 2, on ? ST77XX_YELLOW : 0x52AA);
        }
      } else {                                    // result + green check mark
        cv.setTextColor(ST77XX_GREEN); cv.setCursor(110, y); cv.print(vals[i]);
        cv.drawLine(214, y + 9, 220, y + 15, ST77XX_GREEN);
        cv.drawLine(220, y + 15, 230, y + 2, ST77XX_GREEN);
      }
    }
    tft.drawRGBBitmap(0, 0, cv.getBuffer(), 240, 240);
    delay(40);
  }
  cv.fillScreen(ST77XX_BLACK); gfxStars(cv);
  gfxCenterText(cv, "DIAGNOSTICS", 22, 2, ST77XX_CYAN);
  gfxCenterText(cv, "ALL SYSTEMS", 105, 2, ST77XX_GREEN);
  gfxCenterText(cv, "NOMINAL", 140, 3, ST77XX_GREEN);
  tft.drawRGBBitmap(0, 0, cv.getBuffer(), 240, 240);
  delay(ACTIVITY_HOLD_MS);
}

// SEISMOMETER — scrolling trace stays flat, spikes into a quake, reports it.
static void actSeismo(GFXcanvas16& cv) {
  float mag = 1.0f + random(45) / 10.0f;   // M 1.0 .. 5.4
  int depth = 5 + random(40);
  const int baseY = 122;
  uint32_t t0 = millis();
  while (millis() - t0 < ACTIVITY_RUN_MS) {
    uint32_t e = millis() - t0;
    cv.fillScreen(ST77XX_BLACK); gfxStars(cv);
    gfxCenterText(cv, "SEISMOMETER", 24, 2, COLOR_ORANGE);
    cv.drawFastHLine(0, baseY, 240, 0x4208);
    // amplitude envelope: a faint baseline, a fast attack to a peak, then a
    // smooth decay back down (an attack/decay curve, peaks near 20% through).
    float pr  = e / (float)ACTIVITY_RUN_MS;
    float env = (pr / 0.20f) * expf(1.0f - pr / 0.20f);
    if (env > 1.0f) env = 1.0f;
    float amp = 2.5f + mag * 8.0f * env;
    bool active = env > 0.18f;
    int px = 0, py = baseY;
    for (int x = 0; x < 240; x += 3) {
      float ph = x * 0.18f + e / 40.0f;
      float s  = sinf(ph) + 0.4f * sinf(ph * 2.3f + 1.0f);   // smooth two-tone wave
      int y = baseY + (int)(s * amp);
      if (x > 0) cv.drawLine(px, py, x, y, active ? ST77XX_RED : ST77XX_GREEN);
      px = x; py = y;
    }
    gfxCenterText(cv, active ? "** TREMOR **" : "monitoring", 200, 2,
                  active ? ST77XX_RED : NIGHT_TEXT);
    tft.drawRGBBitmap(0, 0, cv.getBuffer(), 240, 240);
    delay(35);
  }
  cv.fillScreen(ST77XX_BLACK); gfxStars(cv);
  gfxCenterText(cv, "SEISMOMETER", 24, 2, COLOR_ORANGE);
  gfxCenterText(cv, "TREMOR DETECTED", 100, 2, ST77XX_RED);
  char bf[24]; snprintf(bf, sizeof(bf), "M %.1f   %d KM", mag, depth);
  gfxCenterText(cv, bf, 140, 2, ST77XX_WHITE);
  tft.drawRGBBitmap(0, 0, cv.getBuffer(), 240, 240);
  delay(ACTIVITY_HOLD_MS);
}

// PANORAMA — a viewfinder pans the horizon, shutter flash, image stored.
static void actPanorama(GFXcanvas16& cv) {
  const int hY = 150;
  progressInit();
  uint32_t t0 = millis();
  while (millis() - t0 < ACTIVITY_RUN_MS) {
    uint32_t e = millis() - t0;
    cv.fillScreen(ST77XX_BLACK); gfxStars(cv);
    gfxCenterText(cv, "PANORAMA", 24, 2, ST77XX_CYAN);
    cv.drawFastHLine(0, hY, 240, 0x8410);              // horizon + mountains
    cv.fillTriangle(30, hY, 80, hY, 55, hY - 34, 0x4208);
    cv.fillTriangle(95, hY, 165, hY, 130, hY - 50, 0x52AA);
    cv.fillTriangle(160, hY, 225, hY, 193, hY - 30, 0x4208);
    float pp = (float)e / ACTIVITY_RUN_MS;             // panning reticle
    int rx = 20 + (int)(pp * 168), ry = hY - 42, rw = 44, rh = 34;
    cv.drawRect(rx, ry, rw, rh, ST77XX_YELLOW);
    cv.drawFastHLine(rx - 3, ry - 3, 10, ST77XX_WHITE);
    cv.drawFastVLine(rx - 3, ry - 3, 10, ST77XX_WHITE);
    cv.drawFastHLine(rx + rw - 7, ry + rh + 2, 10, ST77XX_WHITE);
    cv.drawFastVLine(rx + rw + 2, ry + rh - 7, 10, ST77XX_WHITE);
    cv.fillCircle(rx + rw / 2, ry + rh / 2, 1, ST77XX_RED);
    char bf[16]; snprintf(bf, sizeof(bf), "SCAN %d%%", progressPct(pp));
    gfxCenterText(cv, bf, 200, 2, ST77XX_WHITE);
    tft.drawRGBBitmap(0, 0, cv.getBuffer(), 240, 240);
    delay(35);
  }
  cv.fillScreen(ST77XX_WHITE);                         // shutter flash
  tft.drawRGBBitmap(0, 0, cv.getBuffer(), 240, 240);
  delay(120);
  cv.fillScreen(ST77XX_BLACK); gfxStars(cv);           // result + thumbnail
  gfxCenterText(cv, "PANORAMA", 24, 2, ST77XX_CYAN);
  int tx = 70, ty = 88, tw = 100, th = 60;
  cv.fillRect(tx, ty, tw, th, 0x10A2); cv.drawRect(tx, ty, tw, th, ST77XX_WHITE);
  cv.drawFastHLine(tx, ty + 40, tw, 0x8410);
  cv.fillTriangle(tx + 12, ty + 40, tx + 46, ty + 40, tx + 30, ty + 16, 0x4208);
  cv.fillTriangle(tx + 48, ty + 40, tx + 92, ty + 40, tx + 70, ty + 8, 0x52AA);
  gfxCenterText(cv, "IMAGE STORED", 175, 2, ST77XX_GREEN);
  tft.drawRGBBitmap(0, 0, cv.getBuffer(), 240, 240);
  delay(ACTIVITY_HOLD_MS);
}

// ---------- scheduler ----------

static uint32_t randRange(uint32_t lo, uint32_t hi) {
  return lo + (uint32_t)random((long)(hi - lo + 1));
}

void scheduleNextActivity() {
  g_next_activity_ms = millis() + randRange(ACTIVITY_IDLE_MIN_MS, ACTIVITY_IDLE_MAX_MS);
}

// Activities eligible for the random idle timer. SOLAR and REACTOR are left out
// — they only fire reactively when actually charging from sun / external power.
static const uint8_t IDLE_POOL[] = {
  ACT_EXCAVATE, ACT_WEATHER, ACT_TRANSMIT, ACT_STARGAZE,
  ACT_DIAG, ACT_SEISMO, ACT_PANORAMA
};
uint8_t pickIdleActivity() { return IDLE_POOL[random(sizeof(IDLE_POOL))]; }

void playActivity(uint8_t a) {
  GFXcanvas16 cv(240, 240);
  switch (a) {
    case ACT_WEATHER:  actWeather(cv);  break;
    case ACT_SOLAR:    actSolar(cv);    break;
    case ACT_REACTOR:  actReactor(cv);  break;
    case ACT_TRANSMIT: actTransmit(cv); break;
    case ACT_STARGAZE: actStargaze(cv); break;
    case ACT_DIAG:     actDiag(cv);     break;
    case ACT_SEISMO:   actSeismo(cv);   break;
    case ACT_PANORAMA: actPanorama(cv); break;
    default:           actExcavate(cv); break;
  }
  g_last_activity_ms = millis();
}

// Repaint the normal UI after an activity hands back the screen.
void afterActivity() { g_static_drawn = false; renderAll(); }

// ---------- connectivity trouble screen ----------
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
