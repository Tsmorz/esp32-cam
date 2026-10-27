// Battery percent from its voltage: the curve's points, in between, and the
// ends
#include "battery_rules.h"
#include <unity.h>

void setUp() {}
void tearDown() {}

void test_points() {
  TEST_ASSERT_EQUAL(100, battery_percent(4200));
  TEST_ASSERT_EQUAL(50, battery_percent(3840));
  TEST_ASSERT_EQUAL(15, battery_percent(3710));
  TEST_ASSERT_EQUAL(0, battery_percent(3270));
}

void test_between() {
  TEST_ASSERT_EQUAL(93, battery_percent(4130)); // 90…95 halfway: 92.5 → 93
  TEST_ASSERT_EQUAL(3, battery_percent(3440));  // 0…5 over 340 mV
}

void test_ends() {
  TEST_ASSERT_EQUAL(100, battery_percent(4350)); // charging
  TEST_ASSERT_EQUAL(0, battery_percent(3000));
  TEST_ASSERT_EQUAL(0, battery_percent(0));
}

void test_never_drops_as_voltage_rises() {
  int last = 0;
  for (int mv = 3000; mv <= 4300; mv++) {
    const int p = battery_percent(mv);
    TEST_ASSERT_TRUE(p >= last && p <= 100);
    last = p;
  }
}

void test_curve_runs_downwards() {
  constexpr int n = sizeof(BATTERY_CURVE) / sizeof(BATTERY_CURVE[0]);
  for (int i = 1; i < n; i++) {
    TEST_ASSERT_TRUE(BATTERY_CURVE[i].mv < BATTERY_CURVE[i - 1].mv);
    TEST_ASSERT_TRUE(BATTERY_CURVE[i].pct < BATTERY_CURVE[i - 1].pct);
  }
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_points);
  RUN_TEST(test_between);
  RUN_TEST(test_ends);
  RUN_TEST(test_never_drops_as_voltage_rises);
  RUN_TEST(test_curve_runs_downwards);
  return UNITY_END();
}
