#include "battery.h"
#include "battery_rules.h"
#include "options.h"
#include <Arduino.h>
#include <atomic>

static constexpr uint32_t READ_MS = 5000;
static constexpr int SAMPLES = 16; // ADC noise is a few mV

static int _pin = -1;
static std::atomic<int> _mv{-1}; // battery mV, smoothed; -1 = not read yet

void battery_init() {
  _pin = options().bat_pin;
  if (_pin < 0)
    return;
  pinMode(_pin, INPUT); // analogReadMilliVolts sets up the ADC on first use
  battery_tick();
}

void battery_tick() {
  static uint32_t last_ms = 0;
  if (_pin < 0 || (_mv >= 0 && millis() - last_ms < READ_MS))
    return;
  last_ms = millis();
  uint32_t sum = 0;
  for (int i = 0; i < SAMPLES; i++)
    sum += analogReadMilliVolts(_pin);
  const int mv = (int)(sum / SAMPLES) * BATTERY_DIVIDER;
  const int prev = _mv;
  // Smoothed, so the camera's current spikes don't make the level jump; a
  // battery plugged in or out shows at once
  const bool step =
      prev < 0 || (prev < BATTERY_NONE_MV) != (mv < BATTERY_NONE_MV);
  _mv = step ? mv : (prev * 3 + mv) / 4;
}

const char *battery_state(int *mv, int *pct) {
  const int v = _mv;
  *mv = v < 0 ? 0 : v;
  *pct = battery_percent(*mv);
  if (_pin < 0)
    return "none";
  if (v < BATTERY_NONE_MV)
    return "missing";
  return *pct <= BATTERY_LOW_PCT ? "low" : "ok";
}
