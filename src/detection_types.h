#pragma once
#include "camera_model.h"
#include <stdint.h>

static constexpr int MAX_TAGS = 8;

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
  uint32_t ms;  // millis() when published; 0 = nothing yet
  uint32_t seq; // processed-frame counter (from 1 at boot), for the rate
  int64_t t_us; // when the camera captured the frame: device clock
                // (esp_timer µs since boot), same clock as `now_us` in the JSON
  int width, height; // source frame size (corner coordinate space)
  CameraModel cam;   // intrinsics used, in source pixels
  int count;
  uint16_t decode_ms, detect_ms; // time spent on this frame
  bool has_imu;   // imu_q is the IMU orientation at t_us (IMU present, warm)
  float imu_q[4]; // [w, x, y, z]
  TagDetection tags[MAX_TAGS];
};
