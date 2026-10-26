// OV5640 band steps from the registers esp32-camera writes per frame size
#include "ov5640_timing.h"
#include <unity.h>

void setUp() {}
void tearDown() {}

// esp32-camera's JPEG PLL: set_pll(false, mult, 4, 2, false, 2, true, 4)
static Ov5640Pll jpeg_pll(uint8_t mult) {
  return {0x1a, 0x41, mult, 0x02, (2 << 4) | 0x06};
}

void test_sysclk_matches_driver() {
  // The driver logs SYSCLK = PLL_CLK / 4 = 20 MHz / 2 × mult / 4 × 2 / 5 / 4
  TEST_ASSERT_EQUAL_UINT32(50000000, ov5640_sysclk_hz(20000000, jpeg_pll(200)));
  TEST_ASSERT_EQUAL_UINT32(45000000, ov5640_sysclk_hz(20000000, jpeg_pll(180)));
  TEST_ASSERT_EQUAL_UINT32(40000000, ov5640_sysclk_hz(20000000, jpeg_pll(160)));
}

void test_sysclk_rejects_nonsense() {
  Ov5640Pll p = jpeg_pll(200);
  p.r3037 = 0x10; // pre-divider 0
  TEST_ASSERT_EQUAL_UINT32(0, ov5640_sysclk_hz(20000000, p));
  TEST_ASSERT_EQUAL_UINT32(0, ov5640_sysclk_hz(20000000, jpeg_pll(0)));
}

void test_band_hd() {
  // 1280×720: binned 16:9, HTS 2844 − 200, VTS 1488 / 2 → 18 910 lines/s
  const Ov5640Band b = ov5640_band(50000000, 2644, 744, false);
  TEST_ASSERT_EQUAL_UINT16(189, b.step50); // 10 ms
  TEST_ASSERT_EQUAL_UINT16(157, b.step60); // 8.33 ms
  TEST_ASSERT_EQUAL_UINT16(744, b.max_expo);
  TEST_ASSERT_EQUAL_UINT8(3, b.max50); // (744 − 4) / 189
  TEST_ASSERT_EQUAL_UINT8(4, b.max60);
}

void test_band_night_longer_but_not_below_min_fps() {
  // SVGA: 45 MHz, HTS 2060, VTS 984 → 21 844 lines/s, 22 fps
  const Ov5640Band day = ov5640_band(45000000, 2060, 984, false);
  const Ov5640Band night = ov5640_band(45000000, 2060, 984, true);
  TEST_ASSERT_EQUAL_UINT16(984, day.max_expo);
  // 3 frames = 2952 lines (7.4 fps) is under the 5 fps floor of 4368
  TEST_ASSERT_EQUAL_UINT16(984 * OV5640_NIGHT_FRAMES, night.max_expo);
  TEST_ASSERT_EQUAL_UINT16(day.step50, night.step50);
  TEST_ASSERT_TRUE(night.max50 > day.max50);

  // UXGA: 50 MHz, HTS 2844, VTS 1968 → 8.9 fps; 5 fps = 3516 lines < 3 frames
  const Ov5640Band full = ov5640_band(50000000, 2844, 1968, true);
  TEST_ASSERT_EQUAL_UINT16(17580 / OV5640_NIGHT_MIN_FPS, full.max_expo);
}

void test_band_night_never_shorter_than_a_frame() {
  // A frame rate already under the floor: keep one frame
  const Ov5640Band b = ov5640_band(10000000, 2844, 1968, true);
  TEST_ASSERT_EQUAL_UINT16(1968, b.max_expo);
}

void test_band_bad_timing_is_zero() {
  const Ov5640Band b = ov5640_band(50000000, 0, 744, false);
  TEST_ASSERT_EQUAL_UINT16(0, b.step50);
  TEST_ASSERT_EQUAL_UINT16(0, ov5640_band(0, 2644, 744, false).step50);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_sysclk_matches_driver);
  RUN_TEST(test_sysclk_rejects_nonsense);
  RUN_TEST(test_band_hd);
  RUN_TEST(test_band_night_longer_but_not_below_min_fps);
  RUN_TEST(test_band_night_never_shorter_than_a_frame);
  RUN_TEST(test_band_bad_timing_is_zero);
  return UNITY_END();
}
