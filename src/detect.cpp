#include "detect.h"
#include "detect_post.h"
#include "options.h"
#include "strbuf.h"
#include <Arduino.h>
#include <atomic>
#include <cstring>
#include <esp_camera.h>
#include <esp_heap_caps.h>
#include <esp_partition.h>
#include <esp_rom_crc.h>
#include <jpeg_decoder.h>
#include <mutex>
#include <strings.h>
#include <tensorflow/lite/micro/micro_interpreter.h>
#include <tensorflow/lite/micro/micro_mutable_op_resolver.h>
#include <tensorflow/lite/micro/system_setup.h>
#include <tensorflow/lite/schema/schema_generated.h>

// ── The stored model ─────────────────────────────────────────────────────────
// Partition "spiffs": [header, 4 KB][model][labels]. The header is written
// last, so a cut-off upload leaves no valid model.
static constexpr uint32_t MAGIC = 0x444D4357; // "WCMD"
static constexpr size_t DATA_AT = 4096, BLOCK = 65536;
struct Header {
  uint32_t magic, model_len, labels_len, crc;
  uint8_t range, pad[3];
  char name[48];
};

static const esp_partition_t *_part = nullptr;
static Header _hdr;
static bool _have = false; // a valid header

static constexpr int MAX_CLASSES = 64, MAX_DETS = 10;
static constexpr int MIN_PERIOD_MS = 700; // leave core 1 some room

// ── Loaded model (task; _mx) ─────────────────────────────────────────────────
static std::mutex _mx; // the loaded model; held during inference and upload
static uint8_t *_model = nullptr, *_arena = nullptr, *_rgb = nullptr;
static size_t _rgb_cap = 0, _arena_size = 0;
static char *_labels_buf = nullptr;
static const char *_labels[MAX_CLASSES];
static int _nlabels = 0;
static tflite::MicroInterpreter *_interp = nullptr;
static InputSpec _in;
static bool _grid = false, _skip_bg = false;
static int _gw = 1, _gh = 1, _ncls = 0;
static float *_scores = nullptr;
static uint8_t *_mark = nullptr;
static int *_stack = nullptr;

// ── Results (_rm) ────────────────────────────────────────────────────────────
struct Result {
  char label[24];
  float score, x, y;
};
static std::mutex _rm;
static Result _res[MAX_DETS];
static int _nres = 0;
static uint32_t _ms = 0, _at = 0; // last inference: duration, millis()
static std::atomic<bool> _trigger{false}, _reload{true}, _uploading{false};
static const char *_state = "none";
static char _why[96] = "";

// Upload in progress (the httpd task)
static size_t _up_total = 0, _up_pos = 0, _erased = 0;
static uint32_t _up_crc = 0;
static Header _up;

static void fail(const char *why) {
  strlcpy(_why, why, sizeof(_why));
  _state = "error";
  Serial.printf("[detect] %s\n", why);
}

static void read_header() {
  _part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                   ESP_PARTITION_SUBTYPE_DATA_SPIFFS, nullptr);
  _have = _part &&
          esp_partition_read(_part, 0, &_hdr, sizeof(_hdr)) == ESP_OK &&
          _hdr.magic == MAGIC &&
          DATA_AT + _hdr.model_len + _hdr.labels_len <= _part->size;
  if (_have)
    _hdr.name[sizeof(_hdr.name) - 1] = '\0';
}

static void unload() {
  delete _interp;
  _interp = nullptr;
  for (void *p : {(void *)_model, (void *)_arena, (void *)_labels_buf,
                  (void *)_scores, (void *)_mark, (void *)_stack})
    heap_caps_free(p);
  _model = _arena = nullptr;
  _labels_buf = nullptr;
  _scores = nullptr;
  _mark = nullptr;
  _stack = nullptr;
  _nlabels = 0;
  std::lock_guard<std::mutex> l(_rm);
  _nres = 0;
}

static void *ps(size_t n) {
  return heap_caps_aligned_alloc(16, n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static TensorType type_of(TfLiteType t) {
  return t == kTfLiteInt8    ? TensorType::Int8
         : t == kTfLiteUInt8 ? TensorType::Uint8
                             : TensorType::Float;
}

static bool load() {
  static tflite::MicroMutableOpResolver<32> ops;
  static bool ops_ready = false;
  if (!ops_ready) {
    tflite::InitializeTarget();
    ops.AddConv2D();
    ops.AddDepthwiseConv2D();
    ops.AddFullyConnected();
    ops.AddSoftmax();
    ops.AddReshape();
    ops.AddAveragePool2D();
    ops.AddMaxPool2D();
    ops.AddAdd();
    ops.AddSub();
    ops.AddMul();
    ops.AddMean();
    ops.AddQuantize();
    ops.AddDequantize();
    ops.AddLogistic();
    ops.AddRelu();
    ops.AddRelu6();
    ops.AddLeakyRelu();
    ops.AddHardSwish();
    ops.AddTanh();
    ops.AddPad();
    ops.AddConcatenation();
    ops.AddStridedSlice();
    ops.AddPack();
    ops.AddShape();
    ops.AddSqueeze();
    ops.AddTranspose();
    ops.AddResizeNearestNeighbor();
    ops.AddMinimum();
    ops.AddMaximum();
    ops_ready = true;
  }
  unload();
  read_header();
  if (!_have) {
    _state = "none";
    return false;
  }
  _state = "loading";
  const size_t n = _hdr.model_len, ln = _hdr.labels_len;
  _model = (uint8_t *)ps(n);
  _labels_buf = (char *)ps(ln + 1);
  if (!_model || !_labels_buf) {
    fail("Not enough memory for this model");
    return false;
  }
  esp_partition_read(_part, DATA_AT, _model, n);
  esp_partition_read(_part, DATA_AT + n, _labels_buf, ln);
  _labels_buf[ln] = '\0';
  uint32_t crc = esp_rom_crc32_le(0, _model, n);
  crc = esp_rom_crc32_le(crc, (const uint8_t *)_labels_buf, ln);
  if (crc != _hdr.crc) {
    fail("The stored model is damaged: upload it again");
    return false;
  }
  for (char *p = _labels_buf; *p && _nlabels < MAX_CLASSES;) {
    char *e = p + strcspn(p, "\r\n");
    const bool last = !*e;
    *e = '\0';
    if (*p)
      _labels[_nlabels++] = p;
    p = last ? e : e + 1;
    while (*p == '\r' || *p == '\n')
      p++;
  }

  const tflite::Model *model = tflite::GetModel(_model);
  if (model->version() != TFLITE_SCHEMA_VERSION) {
    fail("That file isn't a TensorFlow Lite model this firmware understands");
    return false;
  }
  // The working memory a model needs isn't stored in it: try sizes
  for (size_t kb : {256, 512, 1024, 2048, 3072}) {
    _arena = (uint8_t *)ps(kb * 1024);
    if (!_arena)
      break;
    _interp = new tflite::MicroInterpreter(model, ops, _arena, kb * 1024);
    if (_interp->AllocateTensors() == kTfLiteOk) {
      _arena_size = kb * 1024;
      break;
    }
    delete _interp;
    _interp = nullptr;
    heap_caps_free(_arena);
    _arena = nullptr;
  }
  if (!_interp) {
    fail("The model needs an operation this firmware lacks, or too much "
         "memory (see the serial log)");
    return false;
  }

  const TfLiteTensor *in = _interp->input(0);
  if (in->dims->size != 4 ||
      (in->dims->data[3] != 1 && in->dims->data[3] != 3)) {
    fail("The model's input must be a picture (1 × height × width × 1 or 3)");
    return false;
  }
  _in = {in->dims->data[2],     in->dims->data[1], in->dims->data[3],
         type_of(in->type),     in->params.scale,  in->params.zero_point,
         (InputRange)_hdr.range};
  const TfLiteTensor *out = _interp->output(0);
  const int d = out->dims->size;
  if (d == 4 || d == 3) { // [1,]gh,gw,N
    _grid = true;
    _gh = out->dims->data[d - 3];
    _gw = out->dims->data[d - 2];
  } else if (d == 2 || d == 1) {
    _grid = false;
    _gw = _gh = 1;
  } else {
    fail("The model's output isn't a list of classes or a grid of them");
    return false;
  }
  _ncls = out->dims->data[d - 1];
  _skip_bg = _grid || (_nlabels && !strcasecmp(_labels[0], "background"));
  _scores = (float *)ps(sizeof(float) * _gw * _gh * _ncls);
  _mark = (uint8_t *)ps(_gw * _gh);
  _stack = (int *)ps(sizeof(int) * _gw * _gh);
  if (!_scores || !_mark || !_stack) {
    fail("Not enough memory for this model");
    return false;
  }
  _why[0] = '\0';
  _state = "running";
  Serial.printf("[detect] %s: %dx%dx%d in, %s %dx%dx%d out, %u KB arena\n",
                _hdr.name, _in.w, _in.h, _in.c, _grid ? "grid" : "classes", _gw,
                _gh, _ncls, (unsigned)(_arena_size / 1024));
  return true;
}

static const char *label(int cls) {
  static char fallback[16];
  if (cls < _nlabels)
    return _labels[cls];
  snprintf(fallback, sizeof(fallback), "class %d", cls);
  return fallback;
}

// One frame through the model; false if there was nothing to run on
static bool run_once() {
  camera_fb_t *fb = esp_camera_fb_get();
  if (!fb)
    return false;
  const uint32_t t0 = millis();
  // The smallest decode that still covers the model's input
  int s = 8;
  while (s > 1) {
    const Crop c = crop_for(fb->width / s, fb->height / s, _in.w, _in.h);
    if (c.w >= _in.w && c.h >= _in.h)
      break;
    s /= 2;
  }
  const int dw = fb->width / s, dh = fb->height / s;
  const size_t need = (size_t)dw * dh * 3;
  if (need > _rgb_cap) {
    heap_caps_free(_rgb);
    _rgb = (uint8_t *)ps(need);
    _rgb_cap = _rgb ? need : 0;
  }
  esp_jpeg_image_cfg_t cfg = {};
  cfg.indata = fb->buf;
  cfg.indata_size = fb->len;
  cfg.outbuf = _rgb;
  cfg.outbuf_size = _rgb_cap;
  cfg.out_format = JPEG_IMAGE_FORMAT_RGB888;
  cfg.out_scale = s == 8   ? JPEG_IMAGE_SCALE_1_8
                  : s == 4 ? JPEG_IMAGE_SCALE_1_4
                  : s == 2 ? JPEG_IMAGE_SCALE_1_2
                           : JPEG_IMAGE_SCALE_0;
  esp_jpeg_image_output_t img;
  const bool decoded = _rgb && esp_jpeg_decode(&cfg, &img) == ESP_OK;
  const int fw = fb->width, fh = fb->height;
  esp_camera_fb_return(fb); // the camera can have it back
  if (!decoded)
    return false;

  TfLiteTensor *in = _interp->input(0);
  const Crop cr = prep_input(_rgb, img.width, img.height, _in, in->data.raw);
  if (_interp->Invoke() != kTfLiteOk) {
    fail("The model failed to run");
    return false;
  }
  const TfLiteTensor *out = _interp->output(0);
  const int n = _gw * _gh * _ncls;
  for (int i = 0; i < n; i++)
    _scores[i] =
        out->type == kTfLiteFloat32 ? out->data.f[i]
        : out->type == kTfLiteInt8
            ? (out->data.int8[i] - out->params.zero_point) * out->params.scale
            : (out->data.uint8[i] - out->params.zero_point) * out->params.scale;
  const float thr = options().detect_thr / 100.0f;
  Detection d[MAX_DETS];
  const int nd = _grid ? decode_grid(_scores, _gw, _gh, _ncls, thr,
                                     {cr.x0 * s, cr.y0 * s, cr.w * s, cr.h * s},
                                     fw, fh, _mark, _stack, d, MAX_DETS)
                       : decode_classes(_scores, _ncls, thr, _skip_bg, d);
  bool watched = false;
  {
    std::lock_guard<std::mutex> l(_rm);
    _nres = 0;
    for (int i = 0; i < nd; i++) {
      const char *lab = label(d[i].cls);
      if (label_negative(lab)) // "no person", "background": nothing seen
        continue;
      Result &r = _res[_nres++];
      strlcpy(r.label, lab, sizeof(r.label));
      r.score = d[i].score;
      r.x = d[i].x;
      r.y = d[i].y;
      watched |= options_watching(r.label);
    }
    _ms = millis() - t0;
    _at = millis();
  }
  if (watched)
    _trigger = true;
  return true;
}

static void task(void *) {
  for (;;) {
    const bool on = options().detect_on;
    if (_uploading) {
      vTaskDelay(pdMS_TO_TICKS(200));
      continue;
    }
    std::unique_lock<std::mutex> l(_mx);
    if (_reload.exchange(false)) { // a new model, or none
      unload();
      read_header();
      _state = _have ? "off" : "none";
    }
    if (!on) { // off also clears an error, so switching on tries again
      if (_interp)
        unload();
      _state = _have ? "off" : "none";
    } else if (!_interp && !strcmp(_state, "off")) {
      load();
    }
    const bool ran = on && _interp && run_once();
    l.unlock();
    vTaskDelay(pdMS_TO_TICKS(ran ? MIN_PERIOD_MS : 500));
  }
}

void detect_start(BaseType_t core, UBaseType_t prio) {
  read_header();
  _state = _have ? "off" : "none";
  xTaskCreatePinnedToCore(task, "detect", 8192, nullptr, prio, nullptr, core);
}

const char *detect_state() { return _state; }
const char *detect_model_name() { return _have ? _hdr.name : ""; }
bool detect_take_trigger() { return _trigger.exchange(false); }

size_t detect_json(char *buf, size_t size) {
  size_t pos = 0;
  char name[2 * sizeof(_hdr.name)], why[2 * sizeof(_why)];
  json_esc(detect_model_name(), name, sizeof(name));
  json_esc(_why, why, sizeof(why));
  appendf(buf, size, pos,
          "{\"state\":\"%s\",\"why\":\"%s\",\"model\":\"%s\",\"size\":%lu",
          _state, why, name, _have ? (unsigned long)_hdr.model_len : 0UL);
  if (!strcmp(_state, "running"))
    appendf(buf, size, pos,
            ",\"input\":[%d,%d,%d],\"kind\":\"%s\",\"classes\":%d,"
            "\"arena_kb\":%u",
            _in.w, _in.h, _in.c, _grid ? "grid" : "classes", _ncls,
            (unsigned)(_arena_size / 1024));
  if (!strcmp(_state, "running")) { // the dashboard offers them to pick from
    appendf(buf, size, pos, ",\"labels\":[");
    for (int i = 0; i < _nlabels; i++) {
      char lab[48];
      json_esc(_labels[i], lab, sizeof(lab));
      appendf(buf, size, pos, "%s\"%s\"", i ? "," : "", lab);
    }
    appendf(buf, size, pos, "]");
  }
  std::lock_guard<std::mutex> l(_rm);
  appendf(buf, size, pos, ",\"ms\":%lu,\"age_ms\":%lu,\"results\":[",
          (unsigned long)_ms, _at ? (unsigned long)(millis() - _at) : 0UL);
  for (int i = 0; i < _nres; i++) {
    char lab[2 * sizeof(_res[i].label)];
    json_esc(_res[i].label, lab, sizeof(lab));
    appendf(buf, size, pos,
            "%s{\"label\":\"%s\",\"score\":%.2f,\"x\":%.3f,\"y\":%.3f}",
            i ? "," : "", lab, _res[i].score, _res[i].x, _res[i].y);
  }
  appendf(buf, size, pos, "]}");
  return pos;
}

// ── Upload ───────────────────────────────────────────────────────────────────
static bool ensure_erased(size_t end) { // partition offsets [0, end)
  while (_erased < end) {
    if (esp_partition_erase_range(_part, _erased, BLOCK) != ESP_OK)
      return false;
    _erased += BLOCK;
  }
  return true;
}

bool detect_upload_begin(const char *name, size_t model_len, size_t labels_len,
                         int range, const char **why) {
  read_header();
  if (!_part) {
    *why = "No room for a model in this partition table";
    return false;
  }
  if (!model_len || DATA_AT + model_len + labels_len > _part->size) {
    *why = "The model is too big (at most 3.3 MB)";
    return false;
  }
  if (range < 0 || range > 2) {
    *why = "Bad input range";
    return false;
  }
  _uploading = true;
  _mx.lock(); // waits for an inference to finish
  unload();
  _up = {};
  _up.magic = MAGIC;
  _up.model_len = model_len;
  _up.labels_len = labels_len;
  _up.range = (uint8_t)range;
  strlcpy(_up.name, name, sizeof(_up.name));
  _up_total = model_len + labels_len;
  _up_pos = 0;
  _up_crc = 0;
  _erased = 0;
  _have = false;
  _state = "loading";
  if (!ensure_erased(BLOCK)) { // the old header goes first
    *why = "Couldn't erase the model area";
    _uploading = false;
    _mx.unlock();
    return false;
  }
  return true;
}

bool detect_upload_write(const uint8_t *data, size_t n) {
  if (_up_pos + n > _up_total || !ensure_erased(DATA_AT + _up_pos + n) ||
      esp_partition_write(_part, DATA_AT + _up_pos, data, n) != ESP_OK)
    return false;
  _up_crc = esp_rom_crc32_le(_up_crc, data, n);
  _up_pos += n;
  return true;
}

bool detect_upload_end(const char **why) {
  bool ok = _up_pos == _up_total;
  if (!ok)
    *why = "The upload was cut off";
  _up.crc = _up_crc;
  if (ok && esp_partition_write(_part, 0, &_up, sizeof(_up)) != ESP_OK) {
    *why = "Couldn't store the model";
    ok = false;
  }
  _reload = true;
  _uploading = false;
  _mx.unlock();
  Serial.printf("[detect] model %s %s (%u bytes)\n", _up.name,
                ok ? "stored" : "upload failed", (unsigned)_up_total);
  return ok;
}

void detect_forget() {
  std::lock_guard<std::mutex> l(_mx);
  unload();
  read_header();
  if (_part)
    esp_partition_erase_range(_part, 0, DATA_AT);
  _have = false;
  _state = "none";
  _why[0] = '\0';
}
