#include "power.h"
#include "board.h"
#include "camera_profile.h"
#include "imu.h"
#include "options.h"
#include "ota_health.h"
#include "recorder.h"
#include "storage.h"
#include <Arduino.h>
#include <WiFi.h>
#include <atomic>
#include <cstring>
#include <driver/rtc_io.h>
#include <esp_camera.h>
#include <esp_sleep.h>

static const char *_cause = "power";
static std::atomic<uint32_t> _last{0}; // millis() of the last activity

void power_init() {
  switch (esp_sleep_get_wakeup_cause()) {
  case ESP_SLEEP_WAKEUP_EXT0:
    _cause = "motion";
    break;
  case ESP_SLEEP_WAKEUP_TIMER:
    _cause = "timer";
    break;
  default:
    _cause = "power";
  }
  _last = millis();
  if (strcmp(_cause, "power"))
    Serial.printf("[power] woke by %s\n", _cause);
}

const char *power_wake_cause() { return _cause; }

bool power_woke_for_timelapse() {
  return !strcmp(_cause, "timer") && options().timelapse_min > 0;
}

void power_activity() { _last = millis(); }

int power_idle_s() { return (millis() - _last) / 1000; }

const char *power_sleep_blocker() {
  const Options &o = options();
  if (!o.sleep_min)
    return "Sleep is off";
  if (!imu_present())
    return "No motion sensor to wake it";
  if (o.imu_int < 0)
    return "The motion sensor's wake wire isn't set";
  if (ota_health_pending())
    return "Finishing an update";
  if (camera_stream_viewers() > 0)
    return "Someone is watching";
  if (recorder_busy() || recorder_recording())
    return "Saving to the memory card";
  if ((WiFi.getMode() & WIFI_AP) && WiFi.softAPgetStationNum() > 0)
    return "A device is on the camera's WiFi";
  return "";
}

void power_tick() {
  if (!*power_sleep_blocker() && power_idle_s() >= options().sleep_min * 60)
    power_sleep_now();
}

void power_sleep_now() {
  const Options &o = options();
  Serial.println("[power] going to sleep");
  Serial.flush();
  esp_camera_deinit(); // stop capture before its power goes
  board_camera_power_off();
  imu_prepare_wake(o.motion_sens); // no-op after a timelapse wake: still set
  if (o.imu_int >= 0) {
    const gpio_num_t pin = (gpio_num_t)o.imu_int;
    rtc_gpio_pullup_dis(pin);
    rtc_gpio_pulldown_en(pin); // a loose wire must not wake it
    esp_sleep_enable_ext0_wakeup(pin, 1);
  }
  const int32_t tl = recorder_timelapse_in_s();
  if (tl >= 0 && strcmp(storage_state(), "none"))
    esp_sleep_enable_timer_wakeup((uint64_t)(tl < 5 ? 5 : tl) * 1000000ULL);
  WiFi.mode(WIFI_OFF);
  esp_deep_sleep_start();
}
