#include "system.h"
#include <Arduino.h>
#include <atomic>

static std::atomic<uint32_t> _due_ms{0}; // 0 = none
static std::atomic<bool> _pending{false};

void system_restart_in(uint32_t ms) {
  _due_ms = millis() + ms;
  _pending = true;
}

void system_tick() {
  if (_pending && (int32_t)(millis() - _due_ms) >= 0)
    ESP.restart();
}
