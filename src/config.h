#pragma once

// ── WiFi – Station (join existing network) ───────────────────────────────────
#ifndef WIFI_SSID
#define WIFI_SSID "TonyS24"
#endif
#ifndef WIFI_PASS
#define WIFI_PASS "Abc123ts"
#endif

// ── WiFi – Access Point (always-on direct connect) ───────────────────────────
#define AP_SSID "GripLink-Cam"
#define AP_PASS "griplink1"

// ── Battery ADC ──────────────────────────────────────────────────────────────
// GPIO14 has a 2:1 voltage divider on the FireBeetle 2 ESP32-S3.
// Verify against your board schematic; change if readings look wrong.
#define BAT_PIN 14

// ── FireBeetle 2 ESP32-S3 DVP camera pins ────────────────────────────────────
#define CAM_PIN_PWDN  -1
#define CAM_PIN_RESET -1
#define CAM_PIN_XCLK  45
#define CAM_PIN_SIOD   1
#define CAM_PIN_SIOC   2
#define CAM_PIN_D7    48
#define CAM_PIN_D6    46
#define CAM_PIN_D5     8
#define CAM_PIN_D4     7
#define CAM_PIN_D3     4
#define CAM_PIN_D2    41
#define CAM_PIN_D1    40
#define CAM_PIN_D0    39
#define CAM_PIN_VSYNC  6
#define CAM_PIN_HREF  42
#define CAM_PIN_PCLK   5
