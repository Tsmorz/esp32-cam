#include "camera.h"
#include "config.h"
#include "detector.h"
#include "focus.h"
#include "settings.h"
#include <Arduino.h>
#include <Preferences.h>
#include <atomic>
#include <cstring>
#include <esp_camera.h>
#include <freertos/semphr.h>

#define PART_BOUNDARY "123456789000000000000987654321"
static const char *STREAM_CONTENT_TYPE =
    "multipart/x-mixed-replace;boundary=" PART_BOUNDARY;
static const char *STREAM_BOUNDARY = "\r\n--" PART_BOUNDARY "\r\n";
static const char *STREAM_PART =
    "Content-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n";

// Settings that survive a reboot, restored in this order (framesize first:
// changing it re-initialises parts of the sensor)
static const char *const PERSISTED[] = {
    "framesize", "quality",  "brightness", "contrast", "saturation",
    "sharpness", "ae_level", "awb",        "wb_mode",  "aec",
    "agc",       "hmirror",  "vflip",      "af"};

// ── Stream size vs. detection size ───────────────────────────────────────────
// The sensor has one output size, shared by the MJPEG stream and the
// detector, and detection speed depends on it. So the size the user picked for
// the stream (_user_fs) applies while someone is watching; when nobody is and
// a detection size is set (_detect_fs), the sensor drops to that instead.
// Going back to the detection size waits DETECT_HOLD_MS after the last viewer,
// so a burst of snapshots doesn't flip the sensor back and forth.
static constexpr uint32_t DETECT_HOLD_MS = 3000;
static int _user_fs = FRAMESIZE_SVGA;
static int _detect_fs = 0; // 0 = detect at the stream size
static std::atomic<int> _viewers{0};
static std::atomic<uint32_t> _last_view_ms{0};
static SemaphoreHandle_t _size_lock = nullptr;

static bool valid_framesize(int v);

void camera_profile_update() {
  sensor_t *s = esp_camera_sensor_get();
  if (!s || !_size_lock)
    return;
  xSemaphoreTakeRecursive(_size_lock, portMAX_DELAY);
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
  xSemaphoreGiveRecursive(_size_lock);
}

static void viewer_change(int delta) {
  _viewers += delta;
  _last_view_ms = millis();
  camera_profile_update();
}

int camera_detect_size() { return _detect_fs; }

bool camera_set_detect_size(int fs, bool persist) {
  if (fs != 0 && !valid_framesize(fs))
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
  viewer_change(+1);
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
    viewer_change(-1);
  return fb;
}

void camera_fb_return_at_stream_size(camera_fb_t *fb) {
  esp_camera_fb_return(fb);
  viewer_change(-1);
}

bool camera_init() {
  camera_config_t config = {};
  config.ledc_channel = LEDC_CHANNEL_0;
  config.ledc_timer = LEDC_TIMER_0;
  config.pin_d0 = CAM_PIN_D0;
  config.pin_d1 = CAM_PIN_D1;
  config.pin_d2 = CAM_PIN_D2;
  config.pin_d3 = CAM_PIN_D3;
  config.pin_d4 = CAM_PIN_D4;
  config.pin_d5 = CAM_PIN_D5;
  config.pin_d6 = CAM_PIN_D6;
  config.pin_d7 = CAM_PIN_D7;
  config.pin_xclk = CAM_PIN_XCLK;
  config.pin_pclk = CAM_PIN_PCLK;
  config.pin_vsync = CAM_PIN_VSYNC;
  config.pin_href = CAM_PIN_HREF;
  // Join the bus Wire already runs (I2C0) instead of claiming GPIO 1/2 with a
  // second controller — otherwise every other I²C device stops responding.
  config.pin_sccb_sda = -1;
  config.pin_sccb_scl = -1;
  config.sccb_i2c_port = 0;
  config.pin_pwdn = CAM_PIN_PWDN;
  config.pin_reset = CAM_PIN_RESET;
  config.xclk_freq_hz = 20000000; // 10/16 MHz measured worse JPEG artifacts
  config.pixel_format = PIXFORMAT_JPEG;
  // JPEG buffers are sized for the init resolution, so init at the largest the
  // dashboard offers (UXGA) and then drop to SVGA — same as CameraWebServer.
  config.frame_size = FRAMESIZE_UXGA;
  config.jpeg_quality = 12;
  config.fb_count = 2;
  config.fb_location = CAMERA_FB_IN_PSRAM;
  config.grab_mode = CAMERA_GRAB_WHEN_EMPTY;

  if (!_size_lock)
    _size_lock = xSemaphoreCreateRecursiveMutex();
  esp_err_t err = esp_camera_init(&config);
  if (err != ESP_OK) {
    Serial.printf("[cam] init failed: 0x%x\n", err);
    return false;
  }

  sensor_t *s = esp_camera_sensor_get();
  if (s) {
    _user_fs = FRAMESIZE_SVGA;
    s->set_framesize(s, FRAMESIZE_SVGA);
    s->set_whitebal(s, 1);
    s->set_awb_gain(s, 1);
    s->set_exposure_ctrl(s, 1);
    s->set_aec2(s, 1);
    s->set_gain_ctrl(s, 1);
    s->set_bpc(s, 1);
    s->set_wpc(s, 1);
  }

  Serial.printf("[cam] %s ready\n", camera_sensor_name());
  return true;
}

const char *camera_sensor_name() {
  sensor_t *s = esp_camera_sensor_get();
  if (!s)
    return "none";
  camera_sensor_info_t *info = esp_camera_sensor_get_info(&s->id);
  return info ? info->name : "unknown";
}

// The sizes the dashboard offers; all fit the UXGA-sized frame buffers
static bool valid_framesize(int v) {
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

static bool apply(sensor_t *s, const char *var, int val) {
  const bool level = val >= -2 && val <= 2;
  const bool flag = val == 0 || val == 1;
  if (!strcmp(var, "framesize") && valid_framesize(val)) {
    _user_fs = val; // reaches the sensor now, or when a viewer arrives
    camera_profile_update();
    return true;
  }
  if (!strcmp(var, "quality") && val >= 4 && val <= 63)
    return s->set_quality(s, val) == 0;
  if (!strcmp(var, "brightness") && level)
    return s->set_brightness(s, val) == 0;
  if (!strcmp(var, "contrast") && level)
    return s->set_contrast(s, val) == 0;
  if (!strcmp(var, "saturation") && level)
    return s->set_saturation(s, val) == 0;
  if (!strcmp(var, "sharpness") && level)
    return s->set_sharpness(s, val) == 0;
  if (!strcmp(var, "ae_level") && level)
    return s->set_ae_level(s, val) == 0;
  if (!strcmp(var, "awb") && flag)
    return s->set_whitebal(s, val) == 0;
  if (!strcmp(var, "wb_mode") && val >= 0 && val <= 4)
    return s->set_wb_mode(s, val) == 0;
  if (!strcmp(var, "aec") && flag)
    return s->set_exposure_ctrl(s, val) == 0;
  if (!strcmp(var, "agc") && flag)
    return s->set_gain_ctrl(s, val) == 0;
  if (!strcmp(var, "hmirror") && flag)
    return s->set_hmirror(s, val) == 0;
  if (!strcmp(var, "vflip") && flag)
    return s->set_vflip(s, val) == 0;
  if (!strcmp(var, "af"))
    return focus_set_mode(val);
  return false;
}

bool camera_set(const char *var, int val, bool persist) {
  sensor_t *s = esp_camera_sensor_get();
  if (!s)
    return false;
  // Register writes share the SCCB bus with set_framesize(): serialise them.
  // (Recursive: "framesize" re-enters through camera_profile_update. AF only
  // touches the AF MCU and can take seconds, so it stays outside.)
  const bool locked = _size_lock && strcmp(var, "af");
  if (locked)
    xSemaphoreTakeRecursive(_size_lock, portMAX_DELAY);
  const bool ok = apply(s, var, val);
  if (locked)
    xSemaphoreGiveRecursive(_size_lock);
  Serial.printf("[cam] set %s=%d%s\n", var, val, ok ? "" : " (rejected)");
  // A single-shot focus is an action, not a state worth restoring
  if (!ok || !persist || (!strcmp(var, "af") && val == FOCUS_SINGLE))
    return ok;
  for (const char *key : PERSISTED) {
    if (!strcmp(key, var)) {
      Preferences prefs;
      prefs.begin(settings::CAMERA, false);
      prefs.putInt(key, val);
      prefs.end();
      break;
    }
  }
  return ok;
}

void camera_restore() {
  Preferences prefs;
  // The namespaces are independent: `det` can exist without `cam`
  if (prefs.begin(settings::CAMERA, true)) {
    for (const char *key : PERSISTED)
      if (prefs.isKey(key))
        camera_set(key, prefs.getInt(key), false);
    prefs.end();
  }
  if (prefs.begin(settings::DETECT, true)) {
    camera_set_detect_size(prefs.getInt("res", 0), false);
    prefs.end();
  }
}

void camera_reset_settings() {
  settings_reset("camera");
  settings_reset("detector");
}

size_t camera_status_json(char *buf, size_t size) {
  sensor_t *s = esp_camera_sensor_get();
  int n;
  if (!s) {
    n = snprintf(buf, size, "{\"sensor\":\"none\"}");
  } else {
    const camera_status_t &c = s->status;
    n = snprintf(
        buf, size,
        "{\"sensor\":\"%s\",\"framesize\":%d,\"quality\":%d,\"brightness\":%d,"
        "\"contrast\":%d,\"saturation\":%d,\"sharpness\":%d,\"ae_level\":%d,"
        "\"awb\":%d,\"wb_mode\":%d,\"aec\":%d,\"agc\":%d,\"hmirror\":%d,"
        "\"vflip\":%d,\"af\":%d,\"af_state\":\"%s\",\"af_raw\":%d,"
        "\"active_framesize\":%d,\"detect_framesize\":%d,\"viewers\":%d}",
        camera_sensor_name(), _user_fs, c.quality, c.brightness, c.contrast,
        c.saturation, c.sharpness, c.ae_level, c.awb, c.wb_mode, c.aec, c.agc,
        c.hmirror, c.vflip, focus_mode(), focus_state(), focus_raw_status(),
        c.framesize, _detect_fs, _viewers.load());
  }
  if (n < 0)
    return 0;
  return (size_t)n < size ? n : size - 1;
}

static void (*_on_stream_frame)(const camera_fb_t *) = nullptr;

void camera_on_stream_frame(void (*cb)(const camera_fb_t *fb)) {
  _on_stream_frame = cb;
}

esp_err_t stream_handler(httpd_req_t *req) {
  camera_fb_t *fb = nullptr;
  char part_buf[64];
  httpd_resp_set_type(req, STREAM_CONTENT_TYPE);
  httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
  viewer_change(+1); // the sensor moves to the stream size while we watch
  while (true) {
    fb = esp_camera_fb_get();
    if (!fb)
      break;
    if (_on_stream_frame)
      _on_stream_frame(fb);
    httpd_resp_send_chunk(req, STREAM_BOUNDARY, strlen(STREAM_BOUNDARY));
    size_t hlen = snprintf(part_buf, sizeof(part_buf), STREAM_PART, fb->len);
    httpd_resp_send_chunk(req, part_buf, hlen);
    esp_err_t res = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
    esp_camera_fb_return(fb);
    fb = nullptr;
    if (res != ESP_OK)
      break;
  }
  if (fb)
    esp_camera_fb_return(fb);
  viewer_change(-1);
  return ESP_OK;
}
