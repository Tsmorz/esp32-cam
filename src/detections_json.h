#pragma once

// A DetectionFrame as JSON — for /detections (full) and the UDP push (compact:
// no corners, camera model or timings, so 8 tags fit in one datagram).
// No Arduino, tested on the host.

#include "detection_types.h"
#include "strbuf.h"

struct DetectionsJson {
  bool full;       // corners, camera model and timings too
  bool calibrated; // reported in `cam`
  double tag_m;    // printed tag size
  uint32_t now_ms; // millis() — for age_ms
  int64_t now_us;  // esp_timer µs — same clock as t_us
};

static inline size_t detections_json(const DetectionFrame &f,
                                     const DetectionsJson &a, char *buf,
                                     size_t size) {
  size_t pos = 0;
  const long age = f.ms ? (long)(a.now_ms - f.ms) : -1;
  // t_us: capture time; now_us: the device clock at this reply. Their
  // difference is the frame's true age, no clock sync with the board needed.
  appendf(buf, size, pos,
          "{\"seq\":%lu,\"t_us\":%lld,\"now_us\":%lld,\"age_ms\":%ld,\"w\":%d,"
          "\"h\":%d,\"tag_m\":%.4f,",
          (unsigned long)f.seq, (long long)f.t_us, (long long)a.now_us, age,
          f.width, f.height, a.tag_m);
  if (f.has_imu)
    appendf(buf, size, pos, "\"imu_q\":[%.5f,%.5f,%.5f,%.5f],", f.imu_q[0],
            f.imu_q[1], f.imu_q[2], f.imu_q[3]);
  else
    appendf(buf, size, pos, "\"imu_q\":null,");
  if (a.full) {
    const CameraModel &c = f.cam;
    appendf(buf, size, pos,
            "\"cam\":{\"calibrated\":%s,\"fx\":%.2f,\"fy\":%.2f,\"cx\":%.2f,"
            "\"cy\":%.2f,\"dist\":[%.6g,%.6g,%.6g,%.6g,%.6g]},"
            "\"decode_ms\":%u,\"detect_ms\":%u,",
            a.calibrated ? "true" : "false", c.fx, c.fy, c.cx, c.cy, c.dist[0],
            c.dist[1], c.dist[2], c.dist[3], c.dist[4], f.decode_ms,
            f.detect_ms);
  }
  appendf(buf, size, pos, "\"count\":%d,\"tags\":[", f.count);
  for (int i = 0; i < f.count; i++) {
    const TagDetection &t = f.tags[i];
    appendf(buf, size, pos, "%s{\"id\":%d,", i > 0 ? "," : "", t.id);
    if (a.full)
      appendf(buf, size, pos,
              "\"corners\":[[%.1f,%.1f],[%.1f,%.1f],[%.1f,%.1f],[%.1f,%.1f]],",
              t.corners[0][0], t.corners[0][1], t.corners[1][0],
              t.corners[1][1], t.corners[2][0], t.corners[2][1],
              t.corners[3][0], t.corners[3][1]);
    appendf(buf, size, pos,
            "\"pose\":{\"tx\":%.4f,\"ty\":%.4f,\"tz\":%.4f,"
            "\"q\":[%.4f,%.4f,%.4f,%.4f]}}",
            t.tx, t.ty, t.tz, t.q[0], t.q[1], t.q[2], t.q[3]);
  }
  appendf(buf, size, pos, "]}");
  return pos;
}
