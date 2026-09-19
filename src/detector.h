#pragma once
#include <freertos/FreeRTOS.h>
#include <stddef.h>
#include <stdint.h>

static constexpr int MAX_TAGS = 8;

// TAG_SIZE_M: edge of the printed black square in metres (not the white
// border around it). /tag prints at exactly this size.
// Pose accuracy scales linearly with this — measure your tag and update.
static constexpr double TAG_SIZE_M = 0.165;

// Pose frames (AprilTag convention). Camera: x right, y down, z forward.
// Tag: origin at its centre, x right, y down, z into the tag. q rotates tag →
// camera, so a tag facing the camera upright is q = [1, 0, 0, 0].
struct TagDetection {
  int id;
  float corners[4][2]; // [corner_idx][x/y] in source-frame pixels
  double tx, ty, tz;   // tag centre in the camera frame, metres
  float q[4];          // tag orientation in the camera frame, [w, x, y, z]
};

struct DetectionFrame {
  uint32_t ms;       // millis() when published; 0 = nothing yet
  int width, height; // source frame size (corner coordinate space)
  float f;           // focal length in source pixels (principal point centred)
  int count;
  uint16_t decode_ms, detect_ms; // time spent on this frame
  TagDetection tags[MAX_TAGS];
};

bool detector_init();  // allocate buffers + apriltag detector, self-test
bool detector_ready(); // detector_init() succeeded
void detector_start(BaseType_t core, UBaseType_t prio, uint32_t period_ms);
void detector_toggle();
void detector_set_enabled(bool on);
bool detector_enabled();
DetectionFrame detector_latest(); // thread-safe copy
// Last grayscale image the detector ran on, as an 8-bit BMP (caller frees)
size_t detector_gray_bmp(uint8_t **out);
// Printable tag `id` as SVG, black square TAG_SIZE_M wide; 0 if id invalid
size_t detector_tag_svg(int id, char *buf, size_t size);
