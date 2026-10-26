#include "camera_profile.h"
#include "camera_light.h"
#include <Arduino.h>
#include <atomic>
#include <freertos/semphr.h>

static int _user_fs = FRAMESIZE_SVGA;
static std::atomic<int> _viewers{0};
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

int camera_stream_size() { return _user_fs; }

bool camera_set_stream_size(int fs) {
  sensor_t *s = esp_camera_sensor_get();
  if (!s || !camera_valid_framesize(fs))
    return false;
  sensor_lock();
  const bool ok =
      s->status.framesize == fs || s->set_framesize(s, (framesize_t)fs) == 0;
  if (ok)
    camera_light_apply(); // the size set a new PLL and line length
  sensor_unlock();
  if (ok)
    _user_fs = fs;
  return ok;
}

void camera_viewer_change(int delta) { _viewers += delta; }

int camera_stream_viewers() { return _viewers; }

camera_fb_t *camera_fb_get_at_stream_size() {
  const resolution_info_t &want = resolution[_user_fs];
  camera_fb_t *fb = nullptr;
  // After a size change the queued frames (fb_count) are still the old size.
  // Runs on the control httpd task, so it doesn't wait beyond those.
  for (int i = 0; i < 3; i++) {
    fb = esp_camera_fb_get();
    if (!fb || (fb->width == want.width && fb->height == want.height))
      break;
    esp_camera_fb_return(fb);
    fb = nullptr;
  }
  return fb;
}

void camera_fb_return_at_stream_size(camera_fb_t *fb) {
  esp_camera_fb_return(fb);
}
