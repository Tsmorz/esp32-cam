#pragma once

// Calibration ⇄ JSON, for the /calibration route. Plain C, no Arduino, so it
// is unit-tested on a PC. The body is one flat object:
//   {"width":800,"height":600,"fx":..,"fy":..,"cx":..,"cy":..,
//    "dist":[k1,k2,p1,p2,k3]}
// "dist" may be left out (no distortion). Not a general JSON parser: it looks
// each key up by name and reads the number after it.

#include "camera_model.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Pointer just past `"key":` (and any spaces), or null
static inline const char *calibration_json_value(const char *json,
                                                 const char *key) {
  char needle[24];
  snprintf(needle, sizeof(needle), "\"%s\"", key);
  const char *p = strstr(json, needle);
  if (!p)
    return nullptr;
  p += strlen(needle);
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
    p++;
  if (*p != ':')
    return nullptr;
  p++;
  while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')
    p++;
  return p;
}

static inline bool calibration_json_number(const char *json, const char *key,
                                           double &out) {
  const char *p = calibration_json_value(json, key);
  if (!p)
    return false;
  char *end;
  out = strtod(p, &end);
  return end != p;
}

// Fills `out` and returns true only if every intrinsic is present and the
// result passes calibration_valid()
static inline bool calibration_from_json(const char *json, Calibration &out) {
  Calibration c = {};
  double w, h;
  if (!calibration_json_number(json, "width", w) ||
      !calibration_json_number(json, "height", h) ||
      !calibration_json_number(json, "fx", c.fx) ||
      !calibration_json_number(json, "fy", c.fy) ||
      !calibration_json_number(json, "cx", c.cx) ||
      !calibration_json_number(json, "cy", c.cy))
    return false;
  c.width = (int)w;
  c.height = (int)h;
  if (c.width != w || c.height != h)
    return false;

  const char *p = calibration_json_value(json, "dist");
  if (p) {
    if (*p != '[')
      return false;
    p++;
    for (int i = 0; i < 5; i++) {
      char *end;
      c.dist[i] = strtod(p, &end);
      if (end == p)
        return false; // fewer than five numbers
      p = end;
      while (*p == ' ' || *p == ',' || *p == '\n' || *p == '\r' || *p == '\t')
        p++;
    }
    if (*p != ']')
      return false; // more than five
  }
  if (!calibration_valid(c))
    return false;
  out = c;
  return true;
}
