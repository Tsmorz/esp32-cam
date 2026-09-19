#include "ota_health.h"
#include <Arduino.h>
#include <esp_ota_ops.h>

// Overrides Arduino's weak hook (esp32-hal-misc.c): don't mark the image
// valid at startup
extern "C" bool verifyRollbackLater() { return true; }

static bool _pending = true;

bool ota_health_pending() { return _pending; }

void ota_health_tick() {
  if (!_pending || millis() < OTA_CONFIRM_MS)
    return;
  _pending = false;
  const esp_partition_t *running = esp_ota_get_running_partition();
  esp_ota_img_states_t state;
  if (esp_ota_get_state_partition(running, &state) == ESP_OK &&
      state == ESP_OTA_IMG_PENDING_VERIFY) {
    esp_ota_mark_app_valid_cancel_rollback();
    Serial.println("[ota] new firmware confirmed");
  }
}
