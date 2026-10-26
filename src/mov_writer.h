#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

// A QuickTime movie of the camera's JPEG frames (Motion-JPEG, codec 'jpeg'):
// plays in QuickTime on a Mac or iPhone and in VLC, and the dashboard plays
// it by itself. Written as it is recorded:
//
//   ftyp | mdat header (size patched at the end) | frames... | moov
//
// The frames go to the file as they come; their offsets, sizes and durations
// are kept in RAM and become the moov atom (the index) once recording stops.
// Header-only, no Arduino: host-tested.

struct MovFrame {
  uint32_t offset; // in the file
  uint32_t size;
  uint32_t dur_ms;
};

static constexpr size_t MOV_HEAD = 28;         // ftyp (20) + mdat header (8)
static constexpr size_t MOV_MDAT_SIZE_AT = 20; // where the mdat size goes

class MovBuf {
public:
  MovBuf(uint8_t *buf, size_t cap) : _b(buf), _cap(cap) {}
  size_t len() const { return _n; }
  bool ok() const { return _ok; }

  void u8(uint8_t v) { put(&v, 1); }
  void u16(uint16_t v) {
    const uint8_t b[2] = {(uint8_t)(v >> 8), (uint8_t)v};
    put(b, 2);
  }
  void u32(uint32_t v) {
    const uint8_t b[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16),
                          (uint8_t)(v >> 8), (uint8_t)v};
    put(b, 4);
  }
  void tag(const char *t) { put(t, 4); }
  void zeros(size_t n) {
    for (size_t i = 0; i < n; i++)
      u8(0);
  }
  // A Pascal string padded to `field` bytes (0 = just the string)
  void pstr(const char *s, size_t field = 0) {
    const size_t n = strlen(s);
    u8((uint8_t)n);
    put(s, n);
    if (field > n + 1)
      zeros(field - n - 1);
  }
  // Boxes: open() returns where the size goes, close() fills it in
  size_t open(const char *t) {
    const size_t at = _n;
    u32(0);
    tag(t);
    return at;
  }
  void close(size_t at) {
    if (!_ok)
      return;
    const uint32_t sz = (uint32_t)(_n - at);
    _b[at] = sz >> 24;
    _b[at + 1] = sz >> 16;
    _b[at + 2] = sz >> 8;
    _b[at + 3] = sz;
  }

private:
  void put(const void *p, size_t n) {
    if (_n + n > _cap) {
      _ok = false;
      return;
    }
    memcpy(_b + _n, p, n);
    _n += n;
  }
  uint8_t *_b;
  size_t _cap, _n = 0;
  bool _ok = true;
};

// The first MOV_HEAD bytes of the file; the mdat size is patched later
static inline void mov_head(uint8_t out[MOV_HEAD]) {
  MovBuf b(out, MOV_HEAD);
  const size_t ftyp = b.open("ftyp");
  b.tag("qt  ");
  b.u32(0x20050300);
  b.tag("qt  ");
  b.close(ftyp);
  b.u32(8); // mdat: header only so far
  b.tag("mdat");
}

// Big-endian mdat size for the file once `data` bytes of frames follow
static inline void mov_mdat_size(uint32_t data, uint8_t out[4]) {
  const uint32_t v = data + 8;
  out[0] = v >> 24;
  out[1] = v >> 16;
  out[2] = v >> 8;
  out[3] = v;
}

static inline size_t mov_moov_bound(uint32_t frames) {
  return 1024 + 16 * (size_t)frames;
}

static inline void mov_matrix(MovBuf &b) {
  const uint32_t m[9] = {0x10000, 0, 0, 0, 0x10000, 0, 0, 0, 0x40000000};
  for (uint32_t v : m)
    b.u32(v);
}

// The index atom. qt_time: seconds since 1904 (0 if unknown). Returns its
// length, 0 if `out` (mov_moov_bound()) was too small.
static inline size_t mov_moov(uint8_t *out, size_t cap, const MovFrame *f,
                              uint32_t n, uint16_t w, uint16_t h,
                              uint32_t qt_time) {
  uint32_t dur = 0, runs = 0;
  for (uint32_t i = 0; i < n; i++) {
    dur += f[i].dur_ms;
    if (!i || f[i].dur_ms != f[i - 1].dur_ms)
      runs++;
  }
  MovBuf b(out, cap);
  const size_t moov = b.open("moov");

  const size_t mvhd = b.open("mvhd");
  b.u32(0);
  b.u32(qt_time);
  b.u32(qt_time);
  b.u32(1000); // ms
  b.u32(dur);
  b.u32(0x10000); // rate 1.0
  b.u16(0x100);   // volume 1.0
  b.zeros(10);
  mov_matrix(b);
  b.zeros(24); // preview, poster, selection, current time
  b.u32(2);    // next track id
  b.close(mvhd);

  const size_t trak = b.open("trak");
  const size_t tkhd = b.open("tkhd");
  b.u32(0x0000000F); // enabled, in movie, preview, poster
  b.u32(qt_time);
  b.u32(qt_time);
  b.u32(1); // track id
  b.u32(0);
  b.u32(dur);
  b.zeros(8);
  b.u16(0); // layer
  b.u16(0); // alternate group
  b.u16(0); // volume: video
  b.u16(0);
  mov_matrix(b);
  b.u32((uint32_t)w << 16);
  b.u32((uint32_t)h << 16);
  b.close(tkhd);

  const size_t mdia = b.open("mdia");
  const size_t mdhd = b.open("mdhd");
  b.u32(0);
  b.u32(qt_time);
  b.u32(qt_time);
  b.u32(1000);
  b.u32(dur);
  b.u16(0); // language
  b.u16(0); // quality
  b.close(mdhd);
  const size_t hdlr = b.open("hdlr");
  b.u32(0);
  b.tag("mhlr");
  b.tag("vide");
  b.zeros(12);
  b.pstr("VideoHandler");
  b.close(hdlr);

  const size_t minf = b.open("minf");
  const size_t vmhd = b.open("vmhd");
  b.u32(1);
  b.u16(0x40); // graphics mode: dither copy
  b.u16(0x8000);
  b.u16(0x8000);
  b.u16(0x8000);
  b.close(vmhd);
  const size_t dhlr = b.open("hdlr");
  b.u32(0);
  b.tag("dhlr");
  b.tag("alis");
  b.zeros(12);
  b.pstr("DataHandler");
  b.close(dhlr);
  const size_t dinf = b.open("dinf");
  const size_t dref = b.open("dref");
  b.u32(0);
  b.u32(1);
  const size_t alis = b.open("alis");
  b.u32(1); // self-contained
  b.close(alis);
  b.close(dref);
  b.close(dinf);

  const size_t stbl = b.open("stbl");
  const size_t stsd = b.open("stsd");
  b.u32(0);
  b.u32(1);
  const size_t jpeg = b.open("jpeg");
  b.zeros(6);
  b.u16(1); // data reference
  b.u16(0); // version
  b.u16(0); // revision
  b.tag("appl");
  b.u32(0);     // temporal quality
  b.u32(0x200); // spatial quality: normal
  b.u16(w);
  b.u16(h);
  b.u32(0x480000); // 72 dpi
  b.u32(0x480000);
  b.u32(0);
  b.u16(1); // frames per sample
  b.pstr("Photo - JPEG", 32);
  b.u16(24);     // depth
  b.u16(0xFFFF); // no color table
  b.close(jpeg);
  b.close(stsd);

  const size_t stts = b.open("stts"); // durations, run-length coded
  b.u32(0);
  b.u32(runs);
  for (uint32_t i = 0; i < n;) {
    uint32_t j = i;
    while (j < n && f[j].dur_ms == f[i].dur_ms)
      j++;
    b.u32(j - i);
    b.u32(f[i].dur_ms);
    i = j;
  }
  b.close(stts);
  const size_t stsc = b.open("stsc"); // one frame per chunk
  b.u32(0);
  b.u32(1);
  b.u32(1);
  b.u32(1);
  b.u32(1);
  b.close(stsc);
  const size_t stsz = b.open("stsz");
  b.u32(0);
  b.u32(0);
  b.u32(n);
  for (uint32_t i = 0; i < n; i++)
    b.u32(f[i].size);
  b.close(stsz);
  const size_t stco = b.open("stco");
  b.u32(0);
  b.u32(n);
  for (uint32_t i = 0; i < n; i++)
    b.u32(f[i].offset);
  b.close(stco);
  b.close(stbl);

  b.close(minf);
  b.close(mdia);
  b.close(trak);
  b.close(moov);
  return b.ok() ? b.len() : 0;
}
