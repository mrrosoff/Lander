// Pure logic with no Arduino / hardware dependencies, so it can be unit-tested
// on the host (see test/). Keep this header self-contained.
#pragma once
#include <cstddef>

// LiPo discharge curve, mV -> percent. Piecewise-linear from typical single-cell
// curves under light load; monotonic, clamped to 0..100.
inline int batteryPercentFromMv(int mv) {
  struct Point { int mv; int pct; };
  static const Point curve[] = {
    {3300,   0}, {3500,   5}, {3600,  15}, {3700,  30},
    {3800,  50}, {3900,  65}, {4000,  80}, {4100,  90}, {4200, 100},
  };
  const size_t n = sizeof(curve) / sizeof(*curve);
  if (mv <= curve[0].mv)     return 0;
  if (mv >= curve[n - 1].mv) return 100;
  for (size_t i = 1; i < n; i++) {
    if (mv < curve[i].mv) {
      int dv = curve[i].mv  - curve[i - 1].mv;
      int dp = curve[i].pct - curve[i - 1].pct;
      return curve[i - 1].pct + (mv - curve[i - 1].mv) * dp / dv;
    }
  }
  return 100;
}
