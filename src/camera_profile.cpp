#include "camera_profile.h"
#include "detector.h"
#include "settings.h"
#include <Arduino.h>
#include <Preferences.h>
#include <atomic>
#include <freertos/semphr.h>

static constexpr uint32_t DETECT_HOLD_MS = 3000;
static int _user_fs = FRAMESIZE_SVGA;
static int _detect_fs = 0; // 0 = detect at the stream size
static std::atomic<int> _viewers{0};
static std::atomic<uint32_t> _last_view_ms{0};
static SemaphoreHandle_t _lock = nullptr;

void camera_profile_init() {
  if (!_lock)
    _lock = xSemaphoreCreateRecursiveMutex();
}

void sensor_lock() {
  if (_lock)
    xSemaphoreTakeRecursive(_lock, portMAX_DELAY);
}

void sensor_unlock() {
  if (_lock)
    xSemaphoreGiveRecursive(_lock);
}

bool camera_valid_framesize(int v) {
  switch (v) {
  case FRAMESIZE_QQVGA:
  case FRAMESIZE_240X240:
  case FRAMESIZE_QVGA:
  case FRAMESIZE_VGA:
  case FRAMESIZE_SVGA:
  case FRAMESIZE_XGA:
  case FRAMESIZE_HD:
  case FRAMESIZE_UXGA:
    return true;
  default:
    return false;
  }
}

void camera_profile_update() {
  sensor_t *s = esp_camera_sensor_get();
  if (!s || !_lock)
    return;
  sensor_lock();
  const uint32_t seen = _last_view_ms;
  const bool hold = seen && millis() - seen < DETECT_HOLD_MS;
  const int want = (_detect_fs && _viewers == 0 && detector_ready() &&
                    detector_enabled() && !hold)
                       ? _detect_fs
                       : _user_fs;
  if (s->status.framesize != want &&
      s->set_framesize(s, (framesize_t)want) == 0)
    Serial.printf("[cam] frame size -> %d (%s)\n", want,
                  want == _user_fs ? "stream" : "detection");
  sensor_unlock();
}

void camera_viewer_change(int delta) {
  _viewers += delta;
  _last_view_ms = millis();
  camera_profile_update();
}

int camera_stream_size() { return _user_fs; }

bool camera_set_stream_size(int fs) {
  if (!camera_valid_framesize(fs))
    return false;
  _user_fs = fs; // reaches the sensor now, or when a viewer arrives
  camera_profile_update();
  return true;
}

int camera_detect_size() { return _detect_fs; }

bool camera_set_detect_size(int fs, bool persist) {
  if (fs != 0 && !camera_valid_framesize(fs))
    return false;
  _detect_fs = fs;
  if (persist) {
    Preferences prefs;
    prefs.begin(settings::DETECT, false);
    prefs.putInt("res", fs);
    prefs.end();
  }
  Serial.printf("[cam] detection size = %d\n", fs);
  camera_profile_update();
  return true;
}

int camera_stream_viewers() { return _viewers; }

camera_fb_t *camera_fb_get_at_stream_size() {
  camera_viewer_change(+1);
  const resolution_info_t &want = resolution[_user_fs];
  camera_fb_t *fb = nullptr;
  // After a size change the queued frames are still the old size
  for (int i = 0; i < 6; i++) {
    fb = esp_camera_fb_get();
    if (!fb || (fb->width == want.width && fb->height == want.height))
      break;
    esp_camera_fb_return(fb);
    fb = nullptr;
    vTaskDelay(pdMS_TO_TICKS(30));
  }
  if (!fb)
    camera_viewer_change(-1);
  return fb;
}

void camera_fb_return_at_stream_size(camera_fb_t *fb) {
  esp_camera_fb_return(fb);
  camera_viewer_change(-1);
}
