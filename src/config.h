#pragma once

// ── Credentials ──────────────────────────────────────────────────────────────
// Real values live in secrets.h (git-ignored); see secrets.example.h.
#if __has_include("secrets.h")
#include "secrets.h"
#endif

#ifndef WIFI_SSID
#define WIFI_SSID "" // empty → skip STA, AP only
#endif
#ifndef WIFI_PASS
#define WIFI_PASS ""
#endif
#ifndef API_TOKEN
#define API_TOKEN "" // empty → state-changing endpoints are disabled
#endif

// ── WiFi – Access Point (always-on direct connect) ───────────────────────────
#define AP_SSID "GripLink-Cam"
#ifndef AP_PASS
#define AP_PASS "esp32cam1"
#endif

// Reachable as http://<HOSTNAME>.local on the router's network (mDNS)
#define HOSTNAME "esp32cam"

// ── HTTP ─────────────────────────────────────────────────────────────────────
// The MJPEG stream gets its own server: an ESP-IDF httpd instance runs every
// handler on a single task, so an endless stream would block all other routes.
#define HTTP_PORT 80
#define STREAM_PORT 81

// ── Task layout ──────────────────────────────────────────────────────────────
// Core 0 (radio): the SDK already pins WiFi (prio 23), esp_timer (22), lwIP
//   (18) and the camera DMA task here. Both HTTP servers join them, so all
//   network I/O stays on one core and never competes with vision work.
// Core 1 (sensing): the IMU task samples at 104 Hz and preempts the AprilTag
//   detector, which soaks up the rest of the core. Its apriltag worker
//   threads (DETECT_THREADS, same low priority) are unpinned, so they also
//   use core 0's idle time. Arduino loop() (prio 1) only handles serial.
#define NET_CORE 0
#define APP_CORE 1

#define PRIO_HTTP_CTRL 6   // dashboard, JSON, OTA
#define PRIO_HTTP_STREAM 5 // MJPEG
#define PRIO_IMU 10        // short, periodic, must not miss samples
#define PRIO_DETECT 2      // long, CPU-bound, runs whenever the IMU is idle
#define PRIO_LED 1         // status LED, 50 Hz tick
#define PRIO_PUSH 3        // UDP subscriptions: a datagram now and then

#define DETECT_PERIOD_MS 33 // 30 Hz cap; frames are shared with the stream
#define DETECT_THREADS 2    // apriltag workers, unpinned: both cores help

// ── I²C ──────────────────────────────────────────────────────────────────────
// AXP313A, LSM6DSOX and the OV2640's SCCB all share this one bus.
#define I2C_SDA 1
#define I2C_SCL 2
#define I2C_FREQ 400000

// ── Status LED ───────────────────────────────────────────────────────────────
#define LED_PIN 21 // FireBeetle 2 user LED (LED_BUILTIN)

// Which frame the camera driver hands out when several are queued. Latest
// drops the older ones: the stream and the detector see the newest frame, so
// lag stays near one frame interval, at the price of occasionally skipping one.
// 0 = every frame in order (up to fb_count-1 frames of extra lag).
#define CAM_GRAB_LATEST 1

// ── FireBeetle 2 ESP32-S3 DVP camera pins ────────────────────────────────────
// SCCB (SIOD/SIOC) is the shared I²C bus above.
#define CAM_PIN_PWDN -1
#define CAM_PIN_RESET -1
#define CAM_PIN_XCLK 45
#define CAM_PIN_D7 48
#define CAM_PIN_D6 46
#define CAM_PIN_D5 8
#define CAM_PIN_D4 7
#define CAM_PIN_D3 4
#define CAM_PIN_D2 41
#define CAM_PIN_D1 40
#define CAM_PIN_D0 39
#define CAM_PIN_VSYNC 6
#define CAM_PIN_HREF 42
#define CAM_PIN_PCLK 5
