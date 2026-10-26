#include "system.h"
#include "power.h"
#include <Arduino.h>
#include <atomic>

static std::atomic<uint32_t> _due_ms{0};
static std::atomic<bool> _pending{false}, _sleep{false};

void system_restart_in(uint32_t ms) {
  _due_ms = millis() + ms;
  _sleep = false;
  _pending = true;
}

void system_sleep_in(uint32_t ms) {
  _due_ms = millis() + ms;
  _sleep = true;
  _pending = true;
}

void system_tick() {
  if (!_pending || (int32_t)(millis() - _due_ms) < 0)
    return;
  if (_sleep)
    power_sleep_now();
  ESP.restart();
}
