#include "auth.h"
#include "board.h"
#include "camera.h"
#include "config.h"
#include "focus.h"
#include "http_server.h"
#include "imu.h"
#include "led.h"
#include "ota_health.h"
#include "settings.h"
#include "system.h"
#include "wifi_mgr.h"
#include <Arduino.h>
#include <WiFi.h>

static void print_stats() {
  Serial.println("=== stats ===");
  Serial.printf("  ap   : %s  http://%s\n", AP_SSID,
                WiFi.softAPIP().toString().c_str());
  if (WiFi.status() == WL_CONNECTED) {
    Serial.printf("  sta  : %s  http://%s\n", WiFi.SSID().c_str(),
                  WiFi.localIP().toString().c_str());
    Serial.printf("  rssi : %d dBm\n", WiFi.RSSI());
  } else {
    Serial.println("  sta  : not connected");
  }
  Serial.printf("  heap : %u free\n", ESP.getFreeHeap());
  Serial.printf("  psram: %u free\n", ESP.getFreePsram());
  Serial.printf("  temp : %.1f °C (chip)\n", temperatureRead());
  Serial.println("=============");
}

void setup() {
  Serial.begin(115200);
  delay(3000); // wait for USB CDC to enumerate before printing
  Serial.setDebugOutput(true);
  Serial.println("\n[boot] FireBeetle 2 ESP32-S3");
  settings_init();
  auth_init();
  led_start(APP_CORE, PRIO_LED); // fast blink until ready

  board_init(); // I²C bus, camera power

  imu_init(); // LSM6DSOX (+ LIS3MDL); absent → skipped
  const bool cam_ok = camera_init();
  if (cam_ok) {
    focus_init();     // OV5640 only; ~0.2 s to upload the AF firmware
    camera_restore(); // settings saved from the dashboard
  } else {
    Serial.println("[boot] no camera — dashboard, IMU and OTA still run");
  }

  Serial.printf("[boot] WIFI_SSID %s; API token: %s\n",
                WIFI_SSID[0] ? "compiled in"
                             : "none (join one in the dashboard)",
                auth_from_secrets() ? "compiled in"
                : auth_configured() ? "claimed"
                                    : "NOT SET — claim it in the dashboard");
  led_set(LED_WIFI);
  wifi_start();
  http_start();

  imu_start(APP_CORE, PRIO_IMU); // last, so bring-up runs uncontended
  led_set(cam_ok ? LED_IDLE : LED_ERROR);
  Serial.println("[ready]  ?=stats  T=forget-token");
}

void loop() {
#ifdef TEST_CRASH_AFTER_BOOT // scripts/test_ota_rollback.py: a deliberately bad
                             // image
  if (millis() > OTA_CONFIRM_MS - 2000) {
    Serial.println("[test] crashing on purpose");
    abort();
  }
#endif
  focus_tick(); // autofocus mode changes are carried out here, see focus.h
  system_tick();
  if (Serial.available()) {
    char c = Serial.read();
    if (c == '?')
      print_stats();
    if (c == 'T') { // physical access: forget a claimed token
      auth_forget();
      Serial.println("[auth] token forgotten, restarting");
      delay(200);
      ESP.restart();
    }
  }
  static uint32_t last_ms = 0;
  if (millis() - last_ms >= 250) { // housekeeping that isn't worth a task
    last_ms = millis();
    wifi_tick();       // hotspot off while STA is up
    ota_health_tick(); // confirm a new OTA image once it has proved itself
  }
  delay(10);
}
