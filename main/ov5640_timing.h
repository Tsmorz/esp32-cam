#pragma once
#include <stdint.h>

// OV5640 line timing → the AEC's flicker (banding) filter and exposure limit.
// Header-only, no Arduino: host-tested.
//
// Under mains light, brightness pulses at 100 Hz (50 Hz mains) or 120 Hz
// (60 Hz). The sensor reads rows one after another, so unless every exposure
// is a whole number of those pulses, rows get different amounts of light:
// horizontal stripes. The AEC rounds exposure to a "band step" in lines,
// which depends on the line rate = SYSCLK / HTS. esp32-camera writes one fixed
// set of steps at init (295 / 246 lines) and then changes the PLL and HTS per
// frame size, so the steps are wrong at every size — recompute them from the
// live registers (as OmniVision's reference code and Linux's ov5640.c do).

// The PLL registers 0x3034–0x3037 and 0x3108
struct Ov5640Pll {
  uint8_t r3034, r3035, r3036, r3037, r3108;
};

// SYSCLK in Hz, 0 if the registers make no sense
constexpr uint32_t ov5640_sysclk_hz(uint32_t xclk_hz, const Ov5640Pll &p) {
  // 0x3034[3:0]: 8- or 10-bit mode, VCO × 2 / (4 or 5)
  const uint32_t bit_div2x = (p.r3034 & 0x0f) == 8    ? 4
                             : (p.r3034 & 0x0f) == 10 ? 5
                                                      : 1;
  const uint32_t sys_div = (p.r3035 >> 4) ? (p.r3035 >> 4) : 16;
  const uint32_t mult = p.r3036;
  const uint32_t pre_div = p.r3037 & 0x0f; // 5 = 1.5, 7 = 2.5: unused here
  const uint32_t root_div = ((p.r3037 >> 4) & 1) + 1;
  const uint32_t sclk_div = 1u << (p.r3108 & 3);
  if (!pre_div || !mult)
    return 0;
  // In 10 kHz units like the reference code, so 32 bits don't overflow
  const uint32_t vco = xclk_hz / 10000 * mult / pre_div;
  return vco / sys_div / root_div * 2 / bit_div2x / sclk_div * 10000;
}

struct Ov5640Band {
  uint16_t step50, step60; // lines per light pulse (10 ms, 8.33 ms)
  uint8_t max50, max60;    // whole pulses within the longest exposure
  uint16_t max_expo;       // lines; above VTS only in night mode
};

// Low light lets exposure run up to this many frames (frame rate drops by as
// much), but not below this frame rate
static constexpr uint32_t OV5640_NIGHT_FRAMES = 3;
static constexpr uint32_t OV5640_NIGHT_MIN_FPS = 5;

// hts/vts: total line length (pixel clocks) and frame length (lines).
// night: allow exposures longer than a frame (0x3A00 bit 2).
constexpr Ov5640Band ov5640_band(uint32_t sysclk_hz, uint32_t hts, uint32_t vts,
                                 bool night) {
  Ov5640Band b{};
  if (!sysclk_hz || !hts || vts < 8)
    return b;
  const uint32_t lines_s = sysclk_hz / hts;
  uint32_t expo = vts;
  if (night) {
    const uint32_t frames = vts * OV5640_NIGHT_FRAMES;
    const uint32_t floor = lines_s / OV5640_NIGHT_MIN_FPS;
    expo = frames < floor ? frames : floor;
    if (expo < vts)
      expo = vts;
  }
  if (expo > 0xffff)
    expo = 0xffff;
  b.max_expo = expo;
  b.step50 = lines_s / 100;
  b.step60 = lines_s / 120;
  // The reference keeps 4 lines clear of the frame end
  const uint32_t m50 = b.step50 ? (expo - 4) / b.step50 : 0;
  const uint32_t m60 = b.step60 ? (expo - 4) / b.step60 : 0;
  b.max50 = m50 > 255 ? 255 : m50;
  b.max60 = m60 > 255 ? 255 : m60;
  return b;
}
