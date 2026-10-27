#pragma once
#include <stdio.h>
#include <stdlib.h>

// How the picture is shown: turned in quarter turns, mirrored, and
// straightened (four corners of something rectangular on a wall, stretched to
// fill the frame). On screen the picture is Mirror ∘ Turn(turns) of what the
// sensor sees unflipped: mirroring is always left ↔ right as you look at it.
//
// The sensor can only mirror and flip, so it takes everything but a remaining
// quarter turn, which the dashboard does along with the straightening (a CSS
// transform: far too much work per frame for the ESP32). Header-only,
// host-tested (test/test_view).

namespace view {

// A symmetry of the frame, image coordinates (x right, y down) around the
// centre: x' = a x + b y, y' = c x + d y
struct Sym {
  int a, b, c, d;
};
static constexpr Sym IDENTITY = {1, 0, 0, 1};
static constexpr Sym MIRROR = {-1, 0, 0, 1};
static constexpr Sym TURN = {0, -1, 1, 0}; // a quarter turn clockwise

constexpr Sym mul(Sym p, Sym q) { // p after q
  return {p.a * q.a + p.b * q.c, p.a * q.b + p.b * q.d, p.c * q.a + p.d * q.c,
          p.c * q.b + p.d * q.d};
}
constexpr Sym inverse(Sym s) { return {s.a, s.c, s.b, s.d}; } // orthogonal

constexpr Sym turned(int turns) {
  Sym s = IDENTITY;
  for (int i = 0; i < (turns % 4 + 4) % 4; i++)
    s = mul(TURN, s);
  return s;
}

// What the viewer sees, relative to the unflipped sensor
constexpr Sym on_screen(int turns, bool mirror) {
  return mul(mirror ? MIRROR : IDENTITY, turned(turns));
}

struct Split {
  bool hmirror, vflip; // sensor registers
  int quarter;         // 0 or 1: the dashboard turns clockwise by this much
};

// The sensor does on_screen() minus a quarter turn, which is always a
// mirror, a flip, both or neither
constexpr Split split(int turns, bool mirror) {
  const int quarter = (turns % 2 + 2) % 2;
  const Sym s = mul(inverse(turned(quarter)), on_screen(turns, mirror));
  return {s.a < 0, s.d < 0, quarter};
}

// Four corners in thousandths of the frame as shown (after turning and
// mirroring): top left, top right, bottom right, bottom left of the part of
// the picture that should fill the frame
struct Quad {
  bool set = false;
  int p[8] = {}; // x0 y0 x1 y1 x2 y2 x3 y3
};

static constexpr int SCALE = 1000;
// A picture frame across the room can be small: only refuse what's too
// small to be a shape (twice the area, so 0.25 % of the frame)
static constexpr long MIN_AREA2 = 2L * SCALE * SCALE / 400;

// In order round the edge (clockwise on screen), convex, not tiny
inline bool valid(const Quad &q) {
  if (!q.set)
    return true;
  long area2 = 0;
  for (int i = 0; i < 4; i++) {
    const int *a = q.p + 2 * i, *b = q.p + 2 * ((i + 1) % 4),
              *c = q.p + 2 * ((i + 2) % 4);
    if (a[0] < 0 || a[0] > SCALE || a[1] < 0 || a[1] > SCALE)
      return false;
    const long turn = (long)(b[0] - a[0]) * (c[1] - b[1]) -
                      (long)(b[1] - a[1]) * (c[0] - b[0]);
    if (turn <= 0)
      return false;
    area2 += (long)a[0] * b[1] - (long)b[0] * a[1];
  }
  return area2 >= MIN_AREA2;
}

// "x0,y0,x1,y1,x2,y2,x3,y3", or "" for none
inline bool parse(const char *s, Quad &out) {
  Quad q;
  if (*s) {
    q.set = true;
    for (int i = 0; i < 8; i++) {
      char *end;
      const long v = strtol(s, &end, 10);
      if (end == s || v < 0 || v > SCALE || *end != (i < 7 ? ',' : '\0'))
        return false;
      q.p[i] = (int)v;
      s = end + (i < 7);
    }
  }
  if (!valid(q))
    return false;
  out = q;
  return true;
}

inline int format(const Quad &q, char *buf, size_t size) {
  if (!q.set)
    return snprintf(buf, size, "%s", "");
  return snprintf(buf, size, "%d,%d,%d,%d,%d,%d,%d,%d", q.p[0], q.p[1], q.p[2],
                  q.p[3], q.p[4], q.p[5], q.p[6], q.p[7]);
}

// The picture on screen changed by s (turned or mirrored): move the corners
// with it, so the same part of the wall still fills the frame. Corner k goes
// where s takes corner k of the frame.
inline Quad moved(const Quad &q, Sym s) {
  if (!q.set)
    return q;
  static constexpr int FRAME[8] = {-1, -1, 1, -1, 1, 1, -1, 1};
  Quad out;
  out.set = true;
  for (int k = 0; k < 4; k++) {
    const int fx = s.a * FRAME[2 * k] + s.b * FRAME[2 * k + 1],
              fy = s.c * FRAME[2 * k] + s.d * FRAME[2 * k + 1];
    int j = 0;
    while (FRAME[2 * j] != fx || FRAME[2 * j + 1] != fy)
      j++;
    const int x = 2 * q.p[2 * k] - SCALE, y = 2 * q.p[2 * k + 1] - SCALE;
    out.p[2 * j] = (s.a * x + s.b * y + SCALE) / 2;
    out.p[2 * j + 1] = (s.c * x + s.d * y + SCALE) / 2;
  }
  return out;
}

} // namespace view
