#include "board.h"
#include "calibration_store.h"
#include "camera.h"
#include "config.h"
#include "detector.h"
#include "focus.h"
#include "http_server.h"
#include "imu.h"
#include "led.h"
#include "ota_health.h"
#include "push.h"
#include "settings.h"
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
  led_start(APP_CORE, PRIO_LED); // fast blink until ready

  board_init(); // I²C bus, camera power

  imu_init();
  const bool cam_ok = camera_init();
  if (cam_ok) {
    focus_init();     // OV5640 only; ~0.2 s to upload the AF firmware
    camera_restore(); // settings saved from the dashboard
    calibration_load();
    detector_init();
    camera_on_stream_frame(detector_offer);
  } else {
    Serial.println("[boot] no camera — dashboard, IMU and OTA still run");
  }

  led_set(LED_WIFI);
  wifi_start();
  http_start();

  // Tasks start last so bring-up runs uncontended (see Task layout in config.h)
  push_start(NET_CORE, PRIO_PUSH);
  imu_start(APP_CORE, PRIO_IMU);
  if (cam_ok)
    detector_start(APP_CORE, PRIO_DETECT, DETECT_PERIOD_MS);
  led_set(cam_ok ? LED_IDLE : LED_ERROR);
  Serial.println("[ready]  ?=stats  t=toggle-detector");
}

void loop() {
  if (Serial.available()) {
    char c = Serial.read();
    if (c == '?')
      print_stats();
    if (c == 't')
      detector_toggle();
  }
  static uint32_t last_ms = 0;
  if (millis() - last_ms >= 250) { // housekeeping that isn't worth a task
    last_ms = millis();
    camera_profile_update(); // stream/detection size, see camera.h
    wifi_tick();             // hotspot off while STA is up
    ota_health_tick(); // confirm a new OTA image once it has proved itself
  }
  delay(10);
}
