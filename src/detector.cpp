#include "detector.h"
#include "calibration_store.h"
#include "config.h"
#include "esp_jpeg_dec.h"
#include "imu.h"
#include "push.h"
#include "settings.h"
#include "tag_pose.h" // detector settings, pose maths; intrinsics: calibration_store.h
#include <Arduino.h>
#include <Preferences.h>
#include <esp_camera.h>
#include <esp_heap_caps.h>
#include <esp_pthread.h>
#include <freertos/semphr.h>
#include <utility>

// Frames are decoded to luma at the smallest scale (1, 1/2, 1/4, 1/8) that fits
// in DEC_MAX_W × DEC_MAX_H — SVGA → 400×300, UXGA → 400×300, VGA → 320×240.
// AprilTag then finds quads on a 2× decimated copy but reads the tag bits at
// the decoded resolution, which roughly doubles detection range over decoding
// straight to 200×150.
static constexpr int DEC_MAX_W = 400;
static constexpr int DEC_MAX_H = 300;

// Largest JPEG the camera can produce: its buffers are sized for UXGA (see
// camera_init), 1600×1200 / 5.
static constexpr size_t JPEG_BUF = 1600 * 1200 / 5;
// One decoded MCU row (≤ 16 lines) of the widest frame, CbYCrY (2 B/pixel)
static constexpr size_t BLOCK_BUF = 1600 * 16 * 2;

static apriltag_family_t *_family = nullptr;
static apriltag_detector_t *_detector = nullptr;
static uint8_t *_block_buf = nullptr;          // BLOCK_BUF, 16-byte aligned
static SemaphoreHandle_t _gray_lock = nullptr; // decode vs detector_gray_bmp
static uint8_t *_gray_buf = nullptr;           // DEC_MAX_W×DEC_MAX_H grayscale
static uint8_t *_jpeg_copy = nullptr; // JPEG_BUF, the frame being processed
// stream → detector handoff (see detector_offer), JPEG_BUF each
static uint8_t *_stream_spare = nullptr, *_stream_ready = nullptr;
static int _gray_w = 0, _gray_h = 0; // size of the image in _gray_buf

static DetectionFrame _latest = {};
static portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool _enabled = true;
static bool _ready = false; // init and self-test passed
static uint32_t _period_ms = 200;
static DetectorTuning _tuning = DETECTOR_TUNING_DEFAULT; // under _mux

DetectorTuning detector_tuning() {
  portENTER_CRITICAL(&_mux);
  const DetectorTuning t = _tuning;
  portEXIT_CRITICAL(&_mux);
  return t;
}

static bool tuning_valid(const DetectorTuning &t) {
  const float d = t.decimate;
  return (d == 1.0f || d == 1.5f || d == 2.0f || d == 3.0f || d == 4.0f) &&
         t.min_diff >= 1 && t.min_diff <= 100 && t.min_cluster >= 3 &&
         t.min_cluster <= 100;
}

bool detector_set_tuning(const DetectorTuning &t, bool persist) {
  if (!tuning_valid(t))
    return false;
  portENTER_CRITICAL(&_mux);
  _tuning = t;
  portEXIT_CRITICAL(&_mux);
  if (persist) {
    Preferences prefs;
    prefs.begin(settings::DETECT, false);
    prefs.putInt("dec10", (int)(t.decimate * 10 + 0.5f));
    prefs.putInt("diff", t.min_diff);
    prefs.putInt("cluster", t.min_cluster);
    prefs.putBool("refine", t.refine);
    prefs.end();
  }
  Serial.printf("[detect] tuning: decimate %.1f, min_diff %d, min_cluster %d, "
                "refine %d\n",
                t.decimate, t.min_diff, t.min_cluster, (int)t.refine);
  return true;
}

// Between frames, on the detector task: the apriltag detector must not change
// while it runs
static void apply_tuning() {
  const DetectorTuning t = detector_tuning();
  _detector->quad_decimate = t.decimate;
  _detector->qtp.min_white_black_diff = t.min_diff;
  _detector->qtp.min_cluster_pixels = t.min_cluster;
  _detector->refine_edges = t.refine;
}
static void publish(const DetectionFrame &f) {
  portENTER_CRITICAL(&_mux);
  _latest = f;
  _latest.ms = millis();
  if (!_latest.ms)
    _latest.ms = 1; // 0 means "nothing published yet"
  portEXIT_CRITICAL(&_mux);
}

bool detector_init() {
  _gray_lock = xSemaphoreCreateMutex();
  _family = tag36h11_create();
  _detector = apriltag_detector_create();
  if (!configure_detector(_detector, _family)) {
    Serial.println("[detect] tag decode table alloc failed");
    return false;
  }

  _block_buf =
      (uint8_t *)heap_caps_aligned_alloc(16, BLOCK_BUF, MALLOC_CAP_SPIRAM);
  _gray_buf =
      (uint8_t *)heap_caps_malloc(DEC_MAX_W * DEC_MAX_H, MALLOC_CAP_SPIRAM);
  _jpeg_copy = (uint8_t *)heap_caps_malloc(JPEG_BUF, MALLOC_CAP_SPIRAM);
  _stream_spare = (uint8_t *)heap_caps_malloc(JPEG_BUF, MALLOC_CAP_SPIRAM);
  _stream_ready = (uint8_t *)heap_caps_malloc(JPEG_BUF, MALLOC_CAP_SPIRAM);

  if (!_block_buf || !_gray_buf || !_jpeg_copy || !_stream_spare ||
      !_stream_ready) {
    Serial.println("[detect] PSRAM alloc failed");
    return false;
  }
  const uint32_t t0 = millis();
  if (!detector_self_test(_detector, _family)) {
    Serial.println("[detect] self-test failed: synthetic tag not found");
    return false;
  }
  _ready = true;
  Preferences prefs; // saved tuning, after the self-test ran on the defaults
  if (prefs.begin(settings::DETECT, true)) {
    const DetectorTuning d = DETECTOR_TUNING_DEFAULT;
    const DetectorTuning t = {prefs.getInt("dec10", (int)(d.decimate * 10)) /
                                  10.0f,
                              prefs.getInt("diff", d.min_diff),
                              prefs.getInt("cluster", d.min_cluster),
                              prefs.getBool("refine", d.refine)};
    prefs.end();
    detector_set_tuning(t, false);
  }
  Serial.printf("[detect] AprilTag (tag36h11) ready, self-test %lu ms\n",
                (unsigned long)(millis() - t0));
  return true;
}

bool detector_ready() { return _ready; }

static bool pick_scale(int w, int h, int &div) {
  for (div = 1; div <= 8; div *= 2)
    if (w / div <= DEC_MAX_W && h / div <= DEC_MAX_H)
      return true;
  return false;
}

// Luma of one decoded block (CbYCrY: Y is every odd byte) → _gray_buf rows,
// averaging div×div source pixels per output pixel.
static void block_to_gray(const uint8_t *blk, int w, int rows, int div,
                          uint8_t *dst) {
  const int dw = w / div, stride = w * 2;
  if (div == 1) {
    for (int r = 0; r < rows; r++, dst += dw, blk += stride)
      for (int x = 0; x < dw; x++)
        dst[x] = blk[2 * x + 1];
    return;
  }
  const int shift = div == 2 ? 2 : div == 4 ? 4 : 6;
  for (int r = 0; r + div <= rows; r += div, dst += dw, blk += div * stride) {
    for (int x = 0; x < dw; x++) {
      unsigned sum = 0;
      const uint8_t *p = blk + 2 * x * div + 1;
      for (int j = 0; j < div; j++, p += stride - 2 * div)
        for (int i = 0; i < div; i++, p += 2)
          sum += *p;
      dst[x] = sum >> shift;
    }
  }
}

// Decode a JPEG straight to grayscale at 1/div scale, one MCU row at a time.
static bool decode_gray(const uint8_t *jpeg, size_t len, int w, int h,
                        int div) {
  jpeg_dec_config_t cfg = DEFAULT_JPEG_DEC_CONFIG();
  cfg.output_type = JPEG_PIXEL_FORMAT_CbYCrY; // no RGB conversion
  cfg.block_enable = true;
  jpeg_dec_handle_t dec = nullptr;
  if (jpeg_dec_open(&cfg, &dec) != JPEG_ERR_OK)
    return false;

  jpeg_dec_io_t io = {};
  io.inbuf = const_cast<uint8_t *>(jpeg);
  io.inbuf_len = len;
  io.outbuf = _block_buf;
  jpeg_dec_header_info_t info = {};
  int out_len = 0, blocks = 0;
  bool ok = jpeg_dec_parse_header(dec, &io, &info) == JPEG_ERR_OK &&
            info.width == w && info.height == h &&
            jpeg_dec_get_outbuf_len(dec, &out_len) == JPEG_ERR_OK &&
            out_len <= (int)BLOCK_BUF &&
            jpeg_dec_get_process_count(dec, &blocks) == JPEG_ERR_OK;

  int y = 0; // source rows decoded so far
  for (int b = 0; ok && b < blocks; b++) {
    ok = jpeg_dec_process(dec, &io) == JPEG_ERR_OK;
    const int rows = min(io.out_size / (w * 2), h - y);
    if (ok && rows > 0) {
      block_to_gray(_block_buf, w, rows, div,
                    _gray_buf + (y / div) * (w / div));
      y += rows;
    }
  }
  jpeg_dec_close(dec);
  return ok && y == h;
}

static void run(const uint8_t *jpeg, size_t len, int w, int h, int64_t t_us) {
  int div;
  if (!pick_scale(w, h, div)) {
    Serial.printf("[detect] %dx%d too large to decode\n", w, h);
    return;
  }
  const int dw = w / div, dh = h / div;
  const uint32_t t0 = millis();

  apply_tuning();
  xSemaphoreTake(_gray_lock, portMAX_DELAY);
  const bool decoded = decode_gray(jpeg, len, w, h, div);
  if (decoded) {
    _gray_w = dw;
    _gray_h = dh;
  }
  xSemaphoreGive(_gray_lock);
  if (!decoded) {
    Serial.println("[detect] JPEG decode failed");
    return;
  }
  const uint32_t t1 = millis();

  image_u8_t im = {dw, dh, dw, _gray_buf};
  zarray_t *dets = apriltag_detector_detect(_detector, &im);
  const uint32_t t2 = millis();

  // Detection runs on the decoded image, so the intrinsics must be in
  // decoded pixels too.
  const CameraModel cam = camera_model(w, h, calibration_get());

  DetectionFrame out = {};
  static uint32_t seq = 0; // only this task runs run()
  out.seq = ++seq;
  out.t_us = t_us;
  out.has_imu = imu_quat_at(t_us, out.imu_q);
  out.width = w;
  out.height = h;
  out.cam = cam;
  out.decode_ms = t1 - t0;
  out.detect_ms = t2 - t1;
  int n = zarray_size(dets);
  for (int i = 0; i < n && out.count < MAX_TAGS; i++) {
    apriltag_detection_t *det;
    zarray_get(dets, i, &det);

    TagDetection &t = out.tags[out.count++];
    t.id = det->id;
    for (int c = 0; c < 4; c++) { // as seen in the frame, in source pixels
      t.corners[c][0] = (float)(det->p[c][0] * div);
      t.corners[c][1] = (float)(det->p[c][1] * div);
    }

    const TagPose pose = tag_pose(det, cam, div, TAG_SIZE_M);
    t.tx = pose.t[0];
    t.ty = pose.t[1];
    t.tz = pose.t[2];
    memcpy(t.q, pose.q, sizeof(t.q));

    Serial.printf("[detect] tag %3d  t=[%+.3f %+.3f %+.3f] m  "
                  "q=[%.3f %+.3f %+.3f %+.3f]  err=%.4f\n",
                  t.id, t.tx, t.ty, t.tz, t.q[0], t.q[1], t.q[2], t.q[3],
                  pose.err);
  }
  apriltag_detections_destroy(dets);
  if (_enabled) { // toggled off mid-detection: keep the cleared frame
    publish(out);
    push_frame(out);
  }
}

// When the camera captured fb, on the esp_timer clock. The driver stamps the
// first DMA buffer of each frame; if that ever isn't on this clock, fall back
// to now — a little late, but the right clock.
static int64_t capture_time_us(const camera_fb_t *fb) {
  const int64_t now = esp_timer_get_time();
  const int64_t t =
      (int64_t)fb->timestamp.tv_sec * 1000000 + fb->timestamp.tv_usec;
  return (t > 0 && t <= now && now - t < 10000000) ? t : now;
}

// The camera has two frame buffers. While the MJPEG stream runs, taking our
// own frames would cut its frame rate by up to the detection rate, so the
// stream hands every frame over instead (detector_offer); otherwise grab
// directly. Three JPEG buffers: the stream writes _stream_spare, publishes
// it as _stream_ready, and the detector swaps that for _jpeg_copy when it is
// free, so it never waits for the next frame (it may be one frame old).
static constexpr uint32_t STREAM_IDLE_MS = 500; // no frame this long: stopped
static constexpr uint32_t STREAM_WAIT_MS = 200; // then grab our own
static TaskHandle_t _task = nullptr;
static portMUX_TYPE _offer_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile uint32_t _stream_ms = 0;
struct FrameInfo {
  size_t len;
  int w, h;
  int64_t t_us;
};
static FrameInfo _ready_info = {}; // len 0: nothing new
static FrameInfo _spare_info = {};

void detector_offer(const camera_fb_t *fb) {
  _stream_ms = millis();
  if (!_enabled || !_task || fb->len > JPEG_BUF)
    return;
  memcpy(_stream_spare, fb->buf, fb->len); // only this task writes it
  _spare_info = {fb->len, fb->width, fb->height, capture_time_us(fb)};
  portENTER_CRITICAL(&_offer_mux);
  std::swap(_stream_spare, _stream_ready);
  std::swap(_spare_info, _ready_info);
  portEXIT_CRITICAL(&_offer_mux);
  xTaskNotifyGive(_task);
}

// Take the newest stream frame not yet processed, waiting for one if needed;
// false if none comes in time.
static bool frame_from_stream(size_t &len, int &w, int &h, int64_t &t_us) {
  const TickType_t start = xTaskGetTickCount(),
                   wait = pdMS_TO_TICKS(STREAM_WAIT_MS);
  for (;;) {
    portENTER_CRITICAL(&_offer_mux);
    const FrameInfo f = _ready_info;
    if (f.len) {
      std::swap(_stream_ready, _jpeg_copy);
      _ready_info.len = 0;
    }
    portEXIT_CRITICAL(&_offer_mux);
    if (f.len) {
      len = f.len;
      w = f.w;
      h = f.h;
      t_us = f.t_us;
      return true;
    }
    // A notification may be left over from a frame already taken: loop
    const TickType_t waited = xTaskGetTickCount() - start;
    if (waited >= wait || !ulTaskNotifyTake(pdTRUE, wait - waited))
      return false;
  }
}

static bool frame_from_camera(size_t &len, int &w, int &h, int64_t &t_us) {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb)
    return false;
  // Copy the JPEG and hand the buffer straight back
  len = fb->len;
  w = fb->width;
  h = fb->height;
  t_us = capture_time_us(fb);
  const bool ok = len <= JPEG_BUF;
  if (ok)
    memcpy(_jpeg_copy, fb->buf, len);
  esp_camera_fb_return(fb);
  return ok;
}

static void grab() {
  size_t len;
  int w, h;
  int64_t t_us;
  const bool streaming = millis() - _stream_ms < STREAM_IDLE_MS;
  if (!streaming) { // drop a frame left from a stream that has stopped
    portENTER_CRITICAL(&_offer_mux);
    _ready_info.len = 0;
    portEXIT_CRITICAL(&_offer_mux);
  }
  // While streaming, never take frames from the camera's 2-buffer queue: a
  // slow stream just means this cycle has nothing to do.
  if (streaming ? frame_from_stream(len, w, h, t_us)
                : frame_from_camera(len, w, h, t_us))
    run(_jpeg_copy, len, w, h, t_us);
}

static void detector_task(void *) {
  // apriltag's worker threads are pthreads created from this task. The IDF
  // default (2 KB stack, priority 5 — above both HTTP servers) won't do: give
  // them room and this task's priority, on whichever core is idle.
  esp_pthread_cfg_t pcfg = esp_pthread_get_default_config();
  pcfg.stack_size = 8192; // measured peak ~2.5 KB
  pcfg.prio = uxTaskPriorityGet(nullptr);
  pcfg.pin_to_core = tskNO_AFFINITY;
  pcfg.thread_name = "detect_w";
  esp_pthread_set_cfg(&pcfg);
  _detector->nthreads = DETECT_THREADS; // workers are created on first detect

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
  xTaskCreatePinnedToCore(detector_task, "detect", 16384, nullptr, prio, &_task,
                          core);
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
  if (!_gray_buf || !_gray_lock)
    return 0;
  xSemaphoreTake(_gray_lock, portMAX_DELAY);
  const int w = _gray_w, h = _gray_h;
  if (w <= 0 || h <= 0) {
    xSemaphoreGive(_gray_lock);
    return 0;
  }
  const int stride = (w + 3) & ~3; // BMP rows are 4-byte aligned
  const uint32_t hdr = 14 + 40 + 256 * 4;
  const uint32_t size = hdr + stride * h;
  uint8_t *b = (uint8_t *)heap_caps_calloc(1, size, MALLOC_CAP_SPIRAM);
  if (!b) {
    xSemaphoreGive(_gray_lock);
    return 0;
  }
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
  xSemaphoreGive(_gray_lock);
  *out = b;
  return size;
}
