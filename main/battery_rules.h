#pragma once

// Battery level from its voltage: one Li-ion/LiPo cell on the FireBeetle's
// BAT connector, measured through two equal resistors (half the voltage
// reaches the pin; the board has no battery sense of its own while the
// camera uses A2). A resting-voltage curve, piecewise linear — good to about
// 10 %, and a little low while the camera draws current. Header-only, no
// Arduino: host-tested (test/test_battery).

static constexpr int BATTERY_DIVIDER = 2;    // pin voltage × this = battery
static constexpr int BATTERY_NONE_MV = 2500; // below: nothing connected
static constexpr int BATTERY_LOW_PCT = 15;   // "low" from here down

struct BatteryPoint {
  int mv, pct;
};
static constexpr BatteryPoint BATTERY_CURVE[] = {
    {4200, 100}, {4150, 95}, {4110, 90}, {4080, 85}, {4020, 80}, {3980, 75},
    {3950, 70},  {3910, 65}, {3870, 60}, {3850, 55}, {3840, 50}, {3820, 45},
    {3800, 40},  {3790, 35}, {3770, 30}, {3750, 25}, {3730, 20}, {3710, 15},
    {3690, 10},  {3610, 5},  {3270, 0}};

static constexpr int battery_percent(int mv) {
  constexpr int n = sizeof(BATTERY_CURVE) / sizeof(BATTERY_CURVE[0]);
  if (mv >= BATTERY_CURVE[0].mv)
    return 100;
  for (int i = 1; i < n; i++) {
    const BatteryPoint hi = BATTERY_CURVE[i - 1], lo = BATTERY_CURVE[i];
    if (mv >= lo.mv) // round to the nearest percent
      return lo.pct + ((mv - lo.mv) * (hi.pct - lo.pct) * 2 + (hi.mv - lo.mv)) /
                          (2 * (hi.mv - lo.mv));
  }
  return 0;
}
