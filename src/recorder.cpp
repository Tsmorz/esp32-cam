#include "recorder.h"
#include "camera_profile.h"
#include "detect.h"
#include "imu.h"
#include "mov_writer.h"
#include "options.h"
#include "storage.h"
#include <Arduino.h>
#include <atomic>
#include <esp_attr.h>
#include <esp_camera.h>
#include <sys/time.h>
#include <vector>

// After an automatic photo or clip, triggers are ignored this long
static constexpr uint32_t TRIGGER_GAP_MS = 30000;
// Stop a clip with less than this left on the card
static constexpr uint64_t CLIP_RESERVE = 1024 * 1024;
static constexpr uint32_t QT_EPOCH = 2082844800; // 1904 → 1970

static std::atomic<bool> _want_photo{false}, _want_start{false},
    _want_stop{false}, _recording{false}, _busy{false};
static std::atomic<int> _rec_s{0};
static int _start_max_ms = 0;
static const char *_why = "";
static uint32_t _gap_until = 0;

// Seconds on the RTC clock, which keeps counting through deep sleep
static int64_t now_s() {
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  return tv.tv_sec;
}
RTC_DATA_ATTR static int64_t _tl_last = 0; // last timelapse photo, now_s()

static uint32_t qt_time() {
  const time_t t = time(nullptr);
  return t > 1600000000 ? (uint32_t)t + QT_EPOCH : 0;
}

static bool write_photo(const camera_fb_t *fb) {
  char path[40];
  const uint32_t seq = storage_new_file(FileKind::Photo, path, sizeof(path));
  if (!seq) {
    _why = "The memory card is full or missing";
    return false;
  }
  File f = storage_fs().open(path, FILE_WRITE);
  const bool ok = f && f.write(fb->buf, fb->len) == fb->len;
  f.close();
  if (!ok) {
    storage_fs().remove(path);
    _why = "Couldn't write to the memory card";
    return false;
  }
  storage_added(seq, FileKind::Photo, fb->len);
  _why = "";
  Serial.printf("[rec] %s\n", path);
  return true;
}

static bool photo() {
  camera_fb_t *fb = camera_fb_get_at_stream_size();
  if (!fb) {
    _why = "The camera gave no picture";
    return false;
  }
  const bool ok = write_photo(fb);
  camera_fb_return_at_stream_size(fb);
  return ok;
}

static void record(uint32_t max_ms) {
  char path[40];
  const uint32_t seq = storage_new_file(FileKind::Clip, path, sizeof(path));
  File f;
  if (!seq || !(f = storage_fs().open(path, FILE_WRITE))) {
    _why = "The memory card is full or missing";
    return;
  }
  uint8_t head[MOV_HEAD];
  mov_head(head);
  f.write(head, sizeof(head));

  std::vector<MovFrame> frames; // > 4 KB: Arduino puts it in PSRAM
  frames.reserve(max_ms / 1000 * 20);
  uint32_t off = MOV_HEAD;
  uint16_t w = 0, h = 0;
  const uint32_t t0 = millis();
  uint32_t last = t0;
  const uint64_t room = storage_total() - storage_used();
  _recording = true;
  _want_stop = false;
  Serial.printf("[rec] %s started\n", path);
  while (!_want_stop && millis() - t0 < max_ms && off + CLIP_RESERVE < room) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb)
      break;
    const uint32_t now = millis();
    if (!frames.empty())
      frames.back().dur_ms = now - last;
    last = now;
    if (!w) {
      w = fb->width;
      h = fb->height;
    }
    // A size change mid-clip would confuse players: keep the first size
    const bool same = fb->width == w && fb->height == h;
    if (same && f.write(fb->buf, fb->len) != fb->len) {
      _why = "Couldn't write to the memory card";
      esp_camera_fb_return(fb);
      break;
    }
    if (same) {
      frames.push_back({off, (uint32_t)fb->len, 0});
      off += fb->len;
    }
    if (_want_photo.exchange(false))
      write_photo(fb);
    esp_camera_fb_return(fb);
    _rec_s = (now - t0) / 1000;
  }
  _recording = false;
  _rec_s = 0;
  if (frames.empty()) {
    f.close();
    storage_fs().remove(path);
    return;
  }
  frames.back().dur_ms =
      frames.size() > 1 ? frames[frames.size() - 2].dur_ms : 100;
  const size_t cap = mov_moov_bound(frames.size());
  uint8_t *moov = (uint8_t *)ps_malloc(cap);
  const size_t len =
      moov ? mov_moov(moov, cap, frames.data(), frames.size(), w, h, qt_time())
           : 0;
  uint8_t mdat[4];
  mov_mdat_size(off - MOV_HEAD, mdat);
  const bool ok = len && f.write(moov, len) == len &&
                  f.seek(MOV_MDAT_SIZE_AT) && f.write(mdat, 4) == 4;
  free(moov);
  f.close();
  if (!ok) {
    _why = "Couldn't finish the clip";
    storage_fs().remove(path);
    return;
  }
  storage_added(seq, FileKind::Clip, off + len);
  _why = "";
  Serial.printf("[rec] %s: %u frames, %u s\n", path, (unsigned)frames.size(),
                (unsigned)((last - t0) / 1000));
}

int32_t recorder_timelapse_in_s() {
  const int min = options().timelapse_min;
  if (!min)
    return -1;
  const int64_t now = now_s();
  if (!_tl_last || _tl_last > now) // never, or the clock was set since
    _tl_last = now;
  const int64_t left = _tl_last + min * 60 - now;
  return left > 0 ? (int32_t)left : 0;
}

bool recorder_timelapse_now() {
  _tl_last = now_s();
  // The sensor needs a moment for exposure and white balance after power-up
  for (int i = 0; i < 15; i++) {
    camera_fb_t *fb = esp_camera_fb_get();
    if (fb)
      esp_camera_fb_return(fb);
  }
  return photo();
}

// What a trigger does (options().motion_action / detect_action)
static void act(int action) {
  if (action == ACT_PHOTO)
    photo();
  else if (action == ACT_CLIP)
    record(options().clip_s * 1000);
  else
    return;
  _gap_until = millis() + TRIGGER_GAP_MS;
}

static void task(void *) {
  for (;;) {
    vTaskDelay(pdMS_TO_TICKS(50));
    // Always take the flags, so an old event doesn't fire later
    const bool moved = imu_motion_take();
    const bool seen = detect_take_trigger();
    if (!storage_ready()) {
      _want_photo = _want_start = false;
      continue;
    }
    _busy = true;
    if (_want_start.exchange(false))
      record(_start_max_ms);
    if (_want_photo.exchange(false))
      photo();
    if (recorder_timelapse_in_s() == 0) {
      _tl_last = now_s();
      photo();
    }
    if ((int32_t)(millis() - _gap_until) >= 0) {
      if (moved)
        act(options().motion_action);
      else if (seen)
        act(options().detect_action);
    }
    _busy = false;
  }
}

void recorder_start(BaseType_t core, UBaseType_t prio) {
  if (storage_ready() || strcmp(storage_state(), "none"))
    xTaskCreatePinnedToCore(task, "rec", 6144, nullptr, prio, nullptr, core);
}

bool recorder_photo() {
  if (!storage_ready())
    return false;
  _want_photo = true;
  return true;
}

bool recorder_record(bool start) {
  if (!start) {
    _want_stop = true;
    return true;
  }
  if (!storage_ready() || _recording)
    return false;
  _start_max_ms = options().rec_max_min * 60000;
  _want_start = true;
  return true;
}

bool recorder_recording() { return _recording || _want_start; }
int recorder_rec_s() { return _rec_s; }
const char *recorder_why() { return _why; }
bool recorder_busy() { return _busy || _want_photo || _want_start; }
