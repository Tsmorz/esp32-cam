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

// The BOOT button held RESET_HOLD_MS: forget the password, for someone who
// forgot it (no terminal needed). The LED blinks fast once it's held long
// enough; it acts on release, because a reset while GPIO0 is held low starts
// the ROM's USB downloader instead of the firmware.
static void reset_button_tick() {
  static uint32_t down_ms = 0; // 0 = not pressed
  static bool armed = false;
  if (digitalRead(RESET_BUTTON_PIN) == LOW) {
    if (!down_ms)
      down_ms = millis() | 1;
    if (!armed && millis() - down_ms >= RESET_HOLD_MS) {
      armed = true;
      led_set(LED_BOOT);
      Serial.println("[auth] BOOT held: let go to forget the password");
    }
    return;
  }
  down_ms = 0;
  if (!armed)
    return;
  armed = false;
  if (auth_from_secrets()) {
    Serial.println("[auth] the password is compiled in (secrets.h) — kept");
    led_restore();
    return;
  }
  auth_forget();
  Serial.println("[auth] password forgotten (BOOT button), restarting");
  system_restart_in(200);
}

void setup() {
  Serial.begin(115200);
  // Wait for a USB serial monitor, but not when none is connected
  while (!Serial && millis() < 3000)
    delay(10);
  Serial.setDebugOutput(true);
  Serial.println("\n[boot] FireBeetle 2 ESP32-S3");
  ota_health_init();
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
  pinMode(RESET_BUTTON_PIN, INPUT_PULLUP);
  led_set(cam_ok ? LED_IDLE : LED_ERROR);
  ota_health_start();
  Serial.println("[ready]  ?=stats  T=forget-token  BOOT 5 s=forget-token");
}

void loop() {
#ifdef TEST_CRASH_AFTER_BOOT // scripts/test_ota_rollback.py: a deliberately bad
                             // image
  if (ota_health_pending() && ota_health_confirm_s() <= 2) {
    Serial.println("[test] crashing on purpose");
    abort();
  }
#endif
  focus_tick(); // autofocus mode changes are carried out here, see focus.h
  system_tick();
  reset_button_tick();
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
    wifi_tick(); // joins; hotspot off while STA is up
    // Confirm a new OTA image once it has proved itself: servers up, and a
    // network to reach them on
    ota_health_tick(http_running() && (WiFi.status() == WL_CONNECTED ||
                                       (WiFi.getMode() & WIFI_AP)));
  }
  delay(10);
}
