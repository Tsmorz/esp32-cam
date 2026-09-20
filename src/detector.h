#pragma once
#include "detection_types.h"
#include <esp_camera.h>
#include <freertos/FreeRTOS.h>
#include <stddef.h>
#include <stdint.h>

// TAG_SIZE_M: edge of the printed black square in metres (not the white
// border around it). /tag prints at exactly this size.
// Pose accuracy scales linearly with this — measure your tag and update.
static constexpr double TAG_SIZE_M = 0.165;

bool detector_init();  // allocate buffers + apriltag detector, self-test
bool detector_ready(); // detector_init() succeeded
void detector_start(BaseType_t core, UBaseType_t prio, uint32_t period_ms);
// Hand a frame the stream is about to send to the detector, if it is waiting
// for one. While a stream runs the detector takes its frames from here.
void detector_offer(const camera_fb_t *fb);
void detector_toggle();
void detector_set_enabled(bool on);
bool detector_enabled();

// How hard the detector looks, trading range and recall for speed. Persisted
// (NVS `det`) and applied between frames. Detection time is dominated by
// apriltag's quad search, which scales with the decimated image and with how
// many edge candidates a busy scene produces.
struct DetectorTuning {
  float decimate; // quad search runs at 1/decimate size: 1, 1.5, 2, 3 or 4
  int min_diff;   // ignore regions with less black/white contrast (grey levels)
  int min_cluster; // ignore edge clusters shorter than this (pixels)
  bool refine;     // refine quad edges at full resolution
};
// Measured on the board at 320×240 (see README): decimate 4 halves the latency
// of decimate 2 at the cost of range (tag ≥ 24 px across instead of ≥ 16)
static constexpr DetectorTuning DETECTOR_TUNING_DEFAULT = {4.0f, 20, 5, true};
DetectorTuning detector_tuning();
// false if a value is out of range; nothing changes then
bool detector_set_tuning(const DetectorTuning &t, bool persist = true);
DetectionFrame detector_latest(); // thread-safe copy
// Last grayscale image the detector ran on, as an 8-bit BMP (caller frees)
size_t detector_gray_bmp(uint8_t **out);
// Printable tag `id` as SVG, black square TAG_SIZE_M wide; 0 if id invalid
size_t detector_tag_svg(int id, char *buf, size_t size);
