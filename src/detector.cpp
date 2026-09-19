#include "detector.h"
#include "apriltag.h"
#include "apriltag_pose.h"
#include "img_converters.h"
#include "tag36h11.h"
#include <Arduino.h>
#include <cmath>
#include <esp_camera.h>
#include <esp_heap_caps.h>

// Approximate intrinsics: fx ≈ 1.0 × the width of the full 4:3 field of view
// (800 px at SVGA), principal point at the image centre. Good enough for
// sign/magnitude; calibrate with a checkerboard for sub-cm accuracy.
static constexpr double FOCAL_PER_WIDTH = 1.0;

// Focal length in pixels for a w×h image. Other aspect ratios are crops of the
// sensor's 4:3 field: wider ones (16:9, 3:2) keep the full width, narrower
// ones (1:1, 5:4) the full height — e.g. 240×240 sees the FOV of 320×240.
static double focal_px(int w, int h) {
  return FOCAL_PER_WIDTH * fmax(w, h * 4.0 / 3.0);
}

// Rotation matrix → unit quaternion [w, x, y, z], w ≥ 0
static void mat_to_quat(const matd_t *R, float q[4]) {
  const double m00 = MATD_EL(R, 0, 0), m01 = MATD_EL(R, 0, 1),
               m02 = MATD_EL(R, 0, 2), m10 = MATD_EL(R, 1, 0),
               m11 = MATD_EL(R, 1, 1), m12 = MATD_EL(R, 1, 2),
               m20 = MATD_EL(R, 2, 0), m21 = MATD_EL(R, 2, 1),
               m22 = MATD_EL(R, 2, 2);
  double w, x, y, z;
  const double tr = m00 + m11 + m22;
  if (tr > 0) {
    const double s = sqrt(tr + 1.0) * 2;
    w = 0.25 * s, x = (m21 - m12) / s, y = (m02 - m20) / s, z = (m10 - m01) / s;
  } else if (m00 > m11 && m00 > m22) {
    const double s = sqrt(1.0 + m00 - m11 - m22) * 2;
    w = (m21 - m12) / s, x = 0.25 * s, y = (m01 + m10) / s, z = (m02 + m20) / s;
  } else if (m11 > m22) {
    const double s = sqrt(1.0 + m11 - m00 - m22) * 2;
    w = (m02 - m20) / s, x = (m01 + m10) / s, y = 0.25 * s, z = (m12 + m21) / s;
  } else {
    const double s = sqrt(1.0 + m22 - m00 - m11) * 2;
    w = (m10 - m01) / s, x = (m02 + m20) / s, y = (m12 + m21) / s, z = 0.25 * s;
  }
  const double k = (w < 0 ? -1 : 1) / sqrt(w * w + x * x + y * y + z * z);
  q[0] = w * k, q[1] = x * k, q[2] = y * k, q[3] = z * k;
}

// Frames are decoded at the smallest JPEG scale (1, 1/2, 1/4, 1/8) that fits
// in DEC_MAX_W × DEC_MAX_H — SVGA → 400×300, UXGA → 400×300, VGA → 320×240.
// AprilTag then finds quads on a 2× decimated copy but reads the tag bits at
// the decoded resolution, which roughly doubles detection range over decoding
// straight to 200×150.
static constexpr int DEC_MAX_W = 400;
static constexpr int DEC_MAX_H = 300;

// Largest JPEG the camera can produce: its buffers are sized for UXGA (see
// camera_init), 1600×1200 / 5.
static constexpr size_t JPEG_BUF = 1600 * 1200 / 5;

static apriltag_family_t *_family = nullptr;
static apriltag_detector_t *_detector = nullptr;
static uint8_t *_rgb_buf = nullptr;   // DEC_MAX_W×DEC_MAX_H×2  RGB565
static uint8_t *_gray_buf = nullptr;  // DEC_MAX_W×DEC_MAX_H    grayscale
static uint8_t *_jpeg_copy = nullptr; // JPEG_BUF
static int _gray_w = 0, _gray_h = 0;  // size of the image in _gray_buf

static DetectionFrame _latest = {};
static portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool _enabled = true;
static bool _ready = false; // init and self-test passed
static uint32_t _period_ms = 200;

static void publish(const DetectionFrame &f) {
  portENTER_CRITICAL(&_mux);
  _latest = f;
  _latest.ms = millis();
  if (!_latest.ms)
    _latest.ms = 1; // 0 means "nothing published yet"
  portEXIT_CRITICAL(&_mux);
}

// Draw tag 0 on a blank image and detect it. apriltag reports some setup
// failures only by never matching anything, so check before trusting it.
static bool self_test() {
  const int cell = 12, margin = 20;
  image_u8_t *tag = apriltag_to_image(_family, 0); // includes white border
  const int side = tag->width * cell + 2 * margin;
  image_u8_t *im = image_u8_create(side, side);
  memset(im->buf, 255, im->stride * im->height);
  for (int y = 0; y < tag->height * cell; y++)
    for (int x = 0; x < tag->width * cell; x++)
      im->buf[(margin + y) * im->stride + margin + x] =
          tag->buf[(y / cell) * tag->stride + x / cell];

  zarray_t *dets = apriltag_detector_detect(_detector, im);
  bool found = false;
  for (int i = 0; i < zarray_size(dets); i++) {
    apriltag_detection_t *det;
    zarray_get(dets, i, &det);
    found |= det->id == 0;
  }
  apriltag_detections_destroy(dets);
  image_u8_destroy(im);
  image_u8_destroy(tag);
  return found;
}

bool detector_init() {
  _family = tag36h11_create();
  _detector = apriltag_detector_create();
  // Correct up to 1 bit error. The library default (2) builds a 36 MB decode
  // table for tag36h11; the allocation fails on 8 MB PSRAM and apriltag then
  // silently matches nothing. 1 bit needs 1 MB.
  apriltag_detector_add_family_bits(_detector, _family, 1);
  _detector->quad_decimate = 2.0f; // quads at 200×150, bits at 400×300
  _detector->quad_sigma = 0.0f;
  _detector->refine_edges = 1;
  _detector->nthreads = 1;

  if (!_family->impl) {
    Serial.println("[detect] tag decode table alloc failed");
    return false;
  }

  _rgb_buf =
      (uint8_t *)heap_caps_malloc(DEC_MAX_W * DEC_MAX_H * 2, MALLOC_CAP_SPIRAM);
  _gray_buf =
      (uint8_t *)heap_caps_malloc(DEC_MAX_W * DEC_MAX_H, MALLOC_CAP_SPIRAM);
  _jpeg_copy = (uint8_t *)heap_caps_malloc(JPEG_BUF, MALLOC_CAP_SPIRAM);

  if (!_rgb_buf || !_gray_buf || !_jpeg_copy) {
    Serial.println("[detect] PSRAM alloc failed");
    return false;
  }
  const uint32_t t0 = millis();
  if (!self_test()) {
    Serial.println("[detect] self-test failed: synthetic tag not found");
    return false;
  }
  _ready = true;
  Serial.printf("[detect] AprilTag (tag36h11) ready, self-test %lu ms\n",
                (unsigned long)(millis() - t0));
  return true;
}

bool detector_ready() { return _ready; }

static bool pick_scale(int w, int h, esp_jpeg_image_scale_t &scale, int &div) {
  static const struct {
    esp_jpeg_image_scale_t scale;
    int div;
  } opts[] = {{JPG_SCALE_NONE, 1},
              {JPG_SCALE_2X, 2},
              {JPG_SCALE_4X, 4},
              {JPG_SCALE_8X, 8}};
  for (const auto &o : opts) {
    if (w / o.div <= DEC_MAX_W && h / o.div <= DEC_MAX_H) {
      scale = o.scale;
      div = o.div;
      return true;
    }
  }
  return false;
}

static void run(const uint8_t *jpeg, size_t len, int w, int h) {
  esp_jpeg_image_scale_t scale;
  int div;
  if (!pick_scale(w, h, scale, div)) {
    Serial.printf("[detect] %dx%d too large to decode\n", w, h);
    return;
  }
  const int dw = w / div, dh = h / div;
  const uint32_t t0 = millis();

  if (!jpg2rgb565(jpeg, len, _rgb_buf, scale)) {
    Serial.println("[detect] JPEG decode failed");
    return;
  }

  // RGB565 → grayscale (luminance)
  const uint16_t *src = (const uint16_t *)_rgb_buf;
  for (int i = 0; i < dw * dh; i++) {
    uint16_t px = src[i];
    uint8_t r = ((px >> 11) & 0x1F) << 3;
    uint8_t g = ((px >> 5) & 0x3F) << 2;
    uint8_t b = (px & 0x1F) << 3;
    _gray_buf[i] = (uint8_t)((r * 77u + g * 150u + b * 29u) >> 8u);
  }

  _gray_w = dw;
  _gray_h = dh;
  const uint32_t t1 = millis();

  image_u8_t im = {dw, dh, dw, _gray_buf};
  zarray_t *dets = apriltag_detector_detect(_detector, &im);
  const uint32_t t2 = millis();

  // Detection runs on the decoded image, so the intrinsics must be in
  // decoded pixels too.
  const double f = focal_px(dw, dh);
  const double cx = dw / 2.0, cy = dh / 2.0;

  DetectionFrame out = {};
  out.width = w;
  out.height = h;
  out.f = (float)(f * div);
  out.decode_ms = t1 - t0;
  out.detect_ms = t2 - t1;
  int n = zarray_size(dets);
  for (int i = 0; i < n && out.count < MAX_TAGS; i++) {
    apriltag_detection_t *det;
    zarray_get(dets, i, &det);

    TagDetection &t = out.tags[out.count++];
    t.id = det->id;
    for (int c = 0; c < 4; c++) { // back to source-frame pixels
      t.corners[c][0] = (float)(det->p[c][0] * div);
      t.corners[c][1] = (float)(det->p[c][1] * div);
    }

    apriltag_detection_info_t info = {det, TAG_SIZE_M, f, f, cx, cy};
    apriltag_pose_t pose;
    double err = estimate_tag_pose(&info, &pose);
    t.tx = MATD_EL(pose.t, 0, 0);
    t.ty = MATD_EL(pose.t, 1, 0);
    t.tz = MATD_EL(pose.t, 2, 0);
    mat_to_quat(pose.R, t.q);
    matd_destroy(pose.R);
    matd_destroy(pose.t);

    Serial.printf("[detect] tag %3d  t=[%+.3f %+.3f %+.3f] m  "
                  "q=[%.3f %+.3f %+.3f %+.3f]  err=%.4f\n",
                  t.id, t.tx, t.ty, t.tz, t.q[0], t.q[1], t.q[2], t.q[3], err);
  }
  apriltag_detections_destroy(dets);
  if (_enabled) // toggled off mid-detection: keep the cleared frame
    publish(out);
}

static void grab() {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb)
    return;

  // Copy the JPEG and hand the buffer straight back so the stream isn't
  // stalled
  const size_t len = fb->len;
  const int w = fb->width, h = fb->height;
  if (len > JPEG_BUF) {
    esp_camera_fb_return(fb);
    return;
  }
  memcpy(_jpeg_copy, fb->buf, len);
  esp_camera_fb_return(fb);

  run(_jpeg_copy, len, w, h);
}

static void detector_task(void *) {
  TickType_t wake = xTaskGetTickCount();
  for (;;) {
    if (_enabled)
      grab();
    // If detection overran the period, still yield a tick so loop() (lower
    // priority, same core) gets to run.
    if (xTaskDelayUntil(&wake, pdMS_TO_TICKS(_period_ms)) == pdFALSE) {
      vTaskDelay(1);
      wake = xTaskGetTickCount();
    }
  }
}

void detector_start(BaseType_t core, UBaseType_t prio, uint32_t period_ms) {
  if (!_ready)
    return;
  _period_ms = period_ms;
  // Detection used to run in Arduino's 8 KB loop task; give it headroom
  xTaskCreatePinnedToCore(detector_task, "detect", 16384, nullptr, prio,
                          nullptr, core);
}

void detector_set_enabled(bool on) {
  _enabled = on;
  if (!on)
    publish(DetectionFrame{}); // clear, so the overlay drops stale tags
  Serial.printf("[detect] %s\n", on ? "enabled" : "disabled");
}

void detector_toggle() { detector_set_enabled(!_enabled); }

bool detector_enabled() { return _enabled; }

DetectionFrame detector_latest() {
  portENTER_CRITICAL(&_mux);
  DetectionFrame f = _latest;
  portEXIT_CRITICAL(&_mux);
  return f;
}

size_t detector_tag_svg(int id, char *buf, size_t size) {
  if (!_family || id < 0 || id >= (int)_family->ncodes)
    return 0;
  image_u8_t *tag = apriltag_to_image(_family, id);
  // Draw only the black square (the size pose estimation assumes); the paper
  // around it is the white border. One path segment per horizontal run.
  const int n = _family->width_at_border;
  const int b = (_family->total_width - n) / 2;
  const double mm = TAG_SIZE_M * 1000;
  int pos =
      snprintf(buf, size,
               "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%.1fmm\" "
               "height=\"%.1fmm\" viewBox=\"0 0 %d %d\" "
               "shape-rendering=\"crispEdges\"><path d=\"",
               mm, mm, n, n);
  for (int y = 0; y < n; y++) {
    for (int x = 0; x < n;) {
      const uint8_t *row = tag->buf + (b + y) * tag->stride + b;
      if (row[x]) {
        x++;
        continue;
      }
      int run = 1;
      while (x + run < n && !row[x + run])
        run++;
      if (pos > 0 && (size_t)pos < size)
        pos +=
            snprintf(buf + pos, size - pos, "M%d %dh%dv1h-%dz", x, y, run, run);
      x += run;
    }
  }
  if (pos > 0 && (size_t)pos < size)
    pos += snprintf(buf + pos, size - pos, "\"/></svg>");
  image_u8_destroy(tag);
  return pos > 0 && (size_t)pos < size ? pos : 0;
}

size_t detector_gray_bmp(uint8_t **out) {
  const int w = _gray_w, h = _gray_h;
  if (!_gray_buf || w <= 0 || h <= 0)
    return 0;
  const int stride = (w + 3) & ~3; // BMP rows are 4-byte aligned
  const uint32_t hdr = 14 + 40 + 256 * 4;
  const uint32_t size = hdr + stride * h;
  uint8_t *b = (uint8_t *)heap_caps_calloc(1, size, MALLOC_CAP_SPIRAM);
  if (!b)
    return 0;
  auto u16 = [&](int off, uint16_t v) { memcpy(b + off, &v, 2); };
  auto u32 = [&](int off, uint32_t v) { memcpy(b + off, &v, 4); };
  b[0] = 'B';
  b[1] = 'M';
  u32(2, size);
  u32(10, hdr);
  u32(14, 40);
  u32(18, w);
  u32(22, (uint32_t)-h); // negative height = top-down rows
  u16(26, 1);            // planes
  u16(28, 8);            // bits per pixel
  u32(34, stride * h);
  u32(46, 256); // palette entries
  for (int i = 0; i < 256; i++) {
    uint8_t *p = b + 54 + i * 4;
    p[0] = p[1] = p[2] = i;
  }
  for (int y = 0; y < h; y++)
    memcpy(b + hdr + y * stride, _gray_buf + y * w, w);
  *out = b;
  return size;
}
