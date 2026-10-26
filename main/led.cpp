#include "led.h"
#include "config.h"
#include <Arduino.h>

static volatile LedMode _mode = LED_BOOT;
static volatile LedMode _rest = LED_BOOT; // the last mode that isn't LED_OTA

// Alternating on/off durations in ms, starting with "on"; 0 terminates. A
// single "on" entry is steady on.
// Indexed by LedMode — keep in enum order.
static const uint16_t PATTERNS[][5] = {
    {100, 100, 0},            // LED_BOOT
    {500, 500, 0},            // LED_WIFI
    {1000, 0},                // LED_IDLE
    {150, 150, 150, 1550, 0}, // LED_ERROR
    {40, 40, 0},              // LED_OTA
};

static void led_task(void *) {
  pinMode(LED_PIN, OUTPUT);
  int step = 0;
  uint32_t step_start = millis();
  LedMode last = _mode;
  for (;;) {
    const LedMode mode = _mode;
    if (mode != last) {
      last = mode;
      step = 0;
      step_start = millis();
    }
    const uint16_t *p = PATTERNS[mode];
    if (millis() - step_start >= p[step]) {
      step = p[step + 1] ? step + 1 : 0;
      step_start = millis();
    }
    digitalWrite(LED_PIN, step % 2 == 0 ? HIGH : LOW);
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}

void led_start(BaseType_t core, UBaseType_t prio) {
  xTaskCreatePinnedToCore(led_task, "led", 2048, nullptr, prio, nullptr, core);
}

void led_set(LedMode mode) {
  if (mode != LED_OTA)
    _rest = mode;
  _mode = mode;
}

void led_restore() { _mode = _rest; }
