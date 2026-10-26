#include "ota_health.h"
#include <Arduino.h>
#include <esp_ota_ops.h>

// Overrides Arduino's weak hook (esp32-hal-misc.c): don't mark the image
// valid at startup
extern "C" bool verifyRollbackLater() { return true; }

static bool _pending = false, _rolled_back = false;
static uint32_t _start_ms = 0; // 0 = setup() still running

void ota_health_init() {
  const esp_partition_t *running = esp_ota_get_running_partition();
  esp_ota_img_states_t state;
  _pending = esp_ota_get_state_partition(running, &state) == ESP_OK &&
             state == ESP_OTA_IMG_PENDING_VERIFY;
  // The bootloader marks an image that reset on probation invalid; it stays
  // so until that slot is written again, so it only counts as "the last
  // update failed" while it isn't the one running
  const esp_partition_t *bad = esp_ota_get_last_invalid_partition();
  _rolled_back = bad && bad != running;
  if (_pending)
    Serial.println("[ota] new firmware on probation");
  if (_rolled_back)
    Serial.println("[ota] the last update failed; running the previous one");
}

void ota_health_start() { _start_ms = millis() | 1; }

bool ota_health_pending() { return _pending; }

bool ota_health_rolled_back() { return _rolled_back; }

int ota_health_confirm_s() {
  if (!_pending)
    return 0;
  const uint32_t up = _start_ms ? millis() - _start_ms : 0;
  return up >= OTA_CONFIRM_MS ? 0 : (OTA_CONFIRM_MS - up + 999) / 1000;
}

void ota_health_tick(bool healthy) {
  if (!_pending || !_start_ms)
    return;
  const uint32_t up = millis() - _start_ms;
  if (up >= OTA_CONFIRM_MS && healthy) {
    _pending = false;
    esp_ota_mark_app_valid_cancel_rollback();
    Serial.println("[ota] new firmware confirmed");
  } else if (up >= OTA_GIVE_UP_MS) {
    Serial.println("[ota] new firmware never got healthy — rolling back");
    esp_ota_mark_app_invalid_rollback_and_reboot();
  }
}
