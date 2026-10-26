#pragma once
#include <math.h>
#include <stddef.h>
#include <stdint.h>

// The parts of animal detection that don't need the board: turning a camera
// picture into the model's input, and the model's output into detections.
// Header-only, no Arduino: host-tested.
//
// Two kinds of model are understood, as Edge Impulse and similar tools export
// them (TensorFlow Lite, int8/uint8/float):
// - classification: output [N] — what the whole picture shows
// - grid ("FOMO"): output [gh][gw][N], one score per cell and class, class 0
//   = background — where each animal is (a centroid, no box)

enum class TensorType : uint8_t { Float, Int8, Uint8 };
// What the model expects a pixel (0–255) to become before quantization
enum class InputRange : uint8_t {
  Unit = 0,
  Signed = 1,
  Raw = 2
}; // 0..1,
   // -1..1,
   // 0..255

struct InputSpec {
  int w, h, c; // c = 1 (gray) or 3 (RGB)
  TensorType type;
  float scale; // quantization (int types)
  int zero;
  InputRange range;
};

// The centred part of the picture with the model's aspect ratio
struct Crop {
  int x0, y0, w, h;
};

static inline Crop crop_for(int sw, int sh, int dw, int dh) {
  Crop c = {0, 0, sw, sh};
  if ((long)sw * dh > (long)sh * dw) { // wider than the model: trim sides
    c.w = (int)((long)sh * dw / dh);
    c.x0 = (sw - c.w) / 2;
  } else {
    c.h = (int)((long)sw * dh / dw);
    c.y0 = (sh - c.h) / 2;
  }
  return c;
}

static inline float pixel_real(int v, InputRange r) {
  switch (r) {
  case InputRange::Signed:
    return v / 127.5f - 1.0f;
  case InputRange::Raw:
    return (float)v;
  default:
    return v / 255.0f;
  }
}

// src: RGB888, sw × sh. Centre crop, nearest-neighbour resize, convert.
// dst: the input tensor's data (w*h*c elements of in.type).
static inline Crop prep_input(const uint8_t *src, int sw, int sh,
                              const InputSpec &in, void *dst) {
  const Crop cr = crop_for(sw, sh, in.w, in.h);
  int8_t *i8 = (int8_t *)dst;
  uint8_t *u8 = (uint8_t *)dst;
  float *f = (float *)dst;
  size_t o = 0;
  for (int y = 0; y < in.h; y++) {
    const int sy = cr.y0 + (int)((long)y * cr.h / in.h);
    for (int x = 0; x < in.w; x++) {
      const int sx = cr.x0 + (int)((long)x * cr.w / in.w);
      const uint8_t *p = src + 3 * ((size_t)sy * sw + sx);
      int px[3] = {p[0], p[1], p[2]};
      if (in.c == 1)
        px[0] = (px[0] * 299 + px[1] * 587 + px[2] * 114 + 500) / 1000;
      for (int k = 0; k < in.c; k++, o++) {
        const float r = pixel_real(px[k], in.range);
        if (in.type == TensorType::Float) {
          f[o] = r;
          continue;
        }
        long q = lroundf(r / in.scale) + in.zero;
        if (in.type == TensorType::Int8)
          i8[o] = (int8_t)(q < -128 ? -128 : q > 127 ? 127 : q);
        else
          u8[o] = (uint8_t)(q < 0 ? 0 : q > 255 ? 255 : q);
      }
    }
  }
  return cr;
}

// A label that means "nothing to report": background, none, nothing, empty,
// unknown, other, or "no …" ("no person"). Not shown, never a trigger.
static inline bool label_negative(const char *l) {
  static const char *const WORDS[] = {"background", "none",    "nothing",
                                      "empty",      "unknown", "other"};
  const auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; };
  const auto same = [&](const char *a, const char *b) {
    for (; *a && *b; a++, b++)
      if (lower(*a) != lower(*b))
        return false;
    return !*a && !*b;
  };
  for (const char *w : WORDS)
    if (same(l, w))
      return true;
  return lower(l[0]) == 'n' && lower(l[1]) == 'o' &&
         (l[2] == ' ' || l[2] == '_' || l[2] == '-');
}

struct Detection {
  int cls;
  float score;
  float x, y; // centre in the whole picture, 0..1; -1 = whole picture
};

// Classification: the best class, if any is at least thr (skip_bg: class 0
// is "background" and doesn't count)
static inline int decode_classes(const float *s, int n, float thr, bool skip_bg,
                                 Detection *out) {
  int best = -1;
  for (int i = skip_bg ? 1 : 0; i < n; i++)
    if (best < 0 || s[i] > s[best])
      best = i;
  if (best < 0 || s[best] < thr)
    return 0;
  *out = {best, s[best], -1, -1};
  return 1;
}

// Grid: cells of one class at least thr that touch (4-neighbours) are one
// animal; its score is the best cell, its centre the cells' mean. Positions
// are mapped back from the crop to the whole picture (sw × sh). mark:
// gw*gh bytes of scratch.
static inline int decode_grid(const float *s, int gw, int gh, int n, float thr,
                              const Crop &cr, int sw, int sh, uint8_t *mark,
                              int *stack, Detection *out, int max_out) {
  int found = 0;
  for (int cls = 1; cls < n && found < max_out; cls++) {
    for (int i = 0; i < gw * gh; i++)
      mark[i] = 0;
    for (int start = 0; start < gw * gh && found < max_out; start++) {
      if (mark[start] || s[start * n + cls] < thr)
        continue;
      int top = 0, cells = 0;
      float sx = 0, sy = 0, best = 0;
      stack[top++] = start;
      mark[start] = 1;
      while (top) {
        const int c = stack[--top];
        const int cx = c % gw, cy = c / gw;
        const float v = s[c * n + cls];
        best = v > best ? v : best;
        sx += cx + 0.5f;
        sy += cy + 0.5f;
        cells++;
        const int nb[4][2] = {
            {cx - 1, cy}, {cx + 1, cy}, {cx, cy - 1}, {cx, cy + 1}};
        for (const auto &q : nb) {
          if (q[0] < 0 || q[0] >= gw || q[1] < 0 || q[1] >= gh)
            continue;
          const int k = q[1] * gw + q[0];
          if (!mark[k] && s[k * n + cls] >= thr) {
            mark[k] = 1;
            stack[top++] = k;
          }
        }
      }
      const float fx = sx / cells / gw, fy = sy / cells / gh; // in the crop
      out[found++] = {cls, best, (cr.x0 + fx * cr.w) / sw,
                      (cr.y0 + fy * cr.h) / sh};
    }
  }
  return found;
}
