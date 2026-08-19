// Host-side unit tests for the pure logic in src/lander_logic.h.
//   pio test -e native
#include <unity.h>
#include "lander_logic.h"

// ---- batteryPercentFromMv ----
static void test_battery_bounds() {
  TEST_ASSERT_EQUAL(0,   batteryPercentFromMv(3000));   // below curve -> empty
  TEST_ASSERT_EQUAL(0,   batteryPercentFromMv(3300));   // at floor
  TEST_ASSERT_EQUAL(100, batteryPercentFromMv(4200));   // at full
  TEST_ASSERT_EQUAL(100, batteryPercentFromMv(4500));   // above curve -> full
}

static void test_battery_curve_points() {
  TEST_ASSERT_EQUAL(50, batteryPercentFromMv(3800));
  TEST_ASSERT_EQUAL(80, batteryPercentFromMv(4000));
  TEST_ASSERT_EQUAL(90, batteryPercentFromMv(4100));
}

static void test_battery_interpolates() {
  // halfway between 3800(50%) and 3900(65%) -> 57%
  TEST_ASSERT_EQUAL(57, batteryPercentFromMv(3850));
}

static void test_battery_monotonic() {
  int prev = -1;
  for (int mv = 3200; mv <= 4300; mv += 5) {
    int p = batteryPercentFromMv(mv);
    TEST_ASSERT_TRUE(p >= 0 && p <= 100);
    TEST_ASSERT_TRUE(p >= prev);   // never decreases as voltage rises
    prev = p;
  }
}

// ---- wxType (WMO code -> category) ----
static void test_wxtype() {
  TEST_ASSERT_EQUAL(0, wxType(0));    // clear
  TEST_ASSERT_EQUAL(1, wxType(1));    // mainly clear
  TEST_ASSERT_EQUAL(1, wxType(2));    // partly cloudy
  TEST_ASSERT_EQUAL(2, wxType(3));    // overcast
  TEST_ASSERT_EQUAL(2, wxType(45));   // fog
  TEST_ASSERT_EQUAL(3, wxType(61));   // rain
  TEST_ASSERT_EQUAL(3, wxType(81));   // rain showers
  TEST_ASSERT_EQUAL(4, wxType(73));   // snow
  TEST_ASSERT_EQUAL(4, wxType(86));   // snow showers
  TEST_ASSERT_EQUAL(5, wxType(95));   // thunderstorm
  TEST_ASSERT_EQUAL(5, wxType(99));   // thunderstorm w/ hail
  TEST_ASSERT_EQUAL(2, wxType(70));   // gap code -> default cloudy
}

void setUp() {}
void tearDown() {}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_battery_bounds);
  RUN_TEST(test_battery_curve_points);
  RUN_TEST(test_battery_interpolates);
  RUN_TEST(test_battery_monotonic);
  RUN_TEST(test_wxtype);
  return UNITY_END();
}
