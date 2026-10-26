// Picture → model input, and model output → detections
#include "detect_post.h"
#include <unity.h>
#include <vector>

void setUp() {}
void tearDown() {}

void test_crop_keeps_the_models_shape_centred() {
  const Crop c = crop_for(800, 600, 96, 96);
  TEST_ASSERT_EQUAL(600, c.w);
  TEST_ASSERT_EQUAL(600, c.h);
  TEST_ASSERT_EQUAL(100, c.x0);
  TEST_ASSERT_EQUAL(0, c.y0);
  const Crop t = crop_for(400, 600, 1, 1);
  TEST_ASSERT_EQUAL(400, t.h);
  TEST_ASSERT_EQUAL(100, t.y0);
}

void test_prep_quantizes_int8_and_gray() {
  // 3×1 picture: black, white, black; model 1×1 gray int8, 0..1, scale
  // 1/255, zero -128 (Edge Impulse's usual quantization)
  const uint8_t px[9] = {0, 0, 0, 255, 255, 255, 0, 0, 0};
  InputSpec in = {
      1, 1, 1, TensorType::Int8, 1 / 255.0f, -128, InputRange::Unit};
  int8_t out;
  prep_input(px, 3, 1, in, &out); // centre crop 1×1 → the white pixel
  TEST_ASSERT_EQUAL_INT8(127, out);
  InputSpec f = {3, 1, 3, TensorType::Float, 0, 0, InputRange::Signed};
  float rgb[9];
  prep_input(px, 3, 1, f, rgb);
  TEST_ASSERT_FLOAT_WITHIN(1e-5, -1.0f, rgb[0]);
  TEST_ASSERT_FLOAT_WITHIN(1e-5, 1.0f, rgb[5]);
}

void test_classes_skip_background_and_threshold() {
  const float s[3] = {0.9f, 0.05f, 0.05f};
  Detection d;
  TEST_ASSERT_EQUAL(0, decode_classes(s, 3, 0.5f, true, &d));
  const float t[3] = {0.1f, 0.2f, 0.7f};
  TEST_ASSERT_EQUAL(1, decode_classes(t, 3, 0.5f, true, &d));
  TEST_ASSERT_EQUAL(2, d.cls);
  TEST_ASSERT_EQUAL_FLOAT(-1, d.x);
}

void test_grid_merges_touching_cells_into_one_animal() {
  // 4×4 grid, 2 classes (background, cat); a 2×2 cat in the top left and
  // one cell bottom right
  const int gw = 4, gh = 4, n = 2;
  std::vector<float> s(gw * gh * n, 0.0f);
  for (int c : {0, 1, 4, 5, 15})
    s[c * n + 1] = 0.9f;
  uint8_t mark[16];
  int stack[16];
  Detection d[4];
  const Crop full = {0, 0, 100, 100};
  const int k =
      decode_grid(s.data(), gw, gh, n, 0.5f, full, 100, 100, mark, stack, d, 4);
  TEST_ASSERT_EQUAL(2, k);
  TEST_ASSERT_FLOAT_WITHIN(1e-4, 0.25f, d[0].x); // centre of cells 0–1
  TEST_ASSERT_FLOAT_WITHIN(1e-4, 0.25f, d[0].y);
  TEST_ASSERT_FLOAT_WITHIN(1e-4, 0.875f, d[1].x);
  // In a centre crop of a wider picture, x maps back into it
  const Crop mid = {25, 0, 50, 100};
  decode_grid(s.data(), gw, gh, n, 0.5f, mid, 100, 100, mark, stack, d, 4);
  TEST_ASSERT_FLOAT_WITHIN(1e-4, 0.375f, d[0].x);
}

void test_negative_labels() {
  for (const char *l : {"background", "Background", "none", "no person",
                        "No_animal", "no-bird", "unknown", "Other"})
    TEST_ASSERT_TRUE_MESSAGE(label_negative(l), l);
  for (const char *l : {"cat", "noisy miner", "north", "not", "nothing here",
                        "dog", "no", "otter"})
    TEST_ASSERT_FALSE_MESSAGE(label_negative(l), l);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_crop_keeps_the_models_shape_centred);
  RUN_TEST(test_prep_quantizes_int8_and_gray);
  RUN_TEST(test_classes_skip_background_and_threshold);
  RUN_TEST(test_grid_merges_touching_cells_into_one_animal);
  RUN_TEST(test_negative_labels);
  return UNITY_END();
}
