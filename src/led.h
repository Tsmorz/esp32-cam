#pragma once
#include <freertos/FreeRTOS.h>

// User LED as a status light. A tag in view overrides the base pattern
// (solid on), so detection can be checked without the dashboard.
enum LedMode {
  LED_BOOT,  // fast blink: starting up
  LED_WIFI,  // slow blink: joining WiFi
  LED_IDLE,  // heartbeat: running, no tag in view
  LED_ERROR, // double blink: camera failed
  LED_OTA,   // flicker: firmware update in progress
};

void led_start(BaseType_t core, UBaseType_t prio);
void led_set(LedMode mode);
