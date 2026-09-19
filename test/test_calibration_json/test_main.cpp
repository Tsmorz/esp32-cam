// Calibration JSON parsing and validation (the /calibration route body).
// Run with: pio test -e native

#include "calibration_json.h"
#include <unity.h>

void setUp() {}
void tearDown() {}

static const char *GOOD = "{\"width\":800,\"height\":600,\"fx\":812.5,"
                          "\"fy\":810,\"cx\":405.25,\"cy\":297,"
                          "\"dist\":[-0.28, 0.09, 0.001, -0.0015, 0]}";

void test_parses_a_full_calibration() {
  Calibration c = {};
  TEST_ASSERT_TRUE(calibration_from_json(GOOD, c));
  TEST_ASSERT_EQUAL_INT(800, c.width);
  TEST_ASSERT_EQUAL_INT(600, c.height);
  TEST_ASSERT_EQUAL_DOUBLE(812.5, c.fx);
  TEST_ASSERT_EQUAL_DOUBLE(810, c.fy);
  TEST_ASSERT_EQUAL_DOUBLE(405.25, c.cx);
  TEST_ASSERT_EQUAL_DOUBLE(297, c.cy);
  TEST_ASSERT_EQUAL_DOUBLE(-0.28, c.dist[0]);
  TEST_ASSERT_EQUAL_DOUBLE(-0.0015, c.dist[3]);
}

void test_tolerates_whitespace_and_key_order() {
  Calibration c = {};
  const char *j = "{\n \"dist\" : [ 0,0,0,0,0 ],\n \"cy\" : 300,\n"
                  " \"cx\": 400, \"fy\":800, \"fx\":800,\n"
                  " \"height\": 600, \"width\": 800 }";
  TEST_ASSERT_TRUE(calibration_from_json(j, c));
  TEST_ASSERT_EQUAL_DOUBLE(400, c.cx);
  TEST_ASSERT_EQUAL_DOUBLE(300, c.cy);
}

void test_distortion_is_optional() {
  Calibration c = {};
  c.dist[0] = 5; // must be reset, not left over
  const char *j = "{\"width\":800,\"height\":600,\"fx\":800,\"fy\":800,"
                  "\"cx\":400,\"cy\":300}";
  TEST_ASSERT_TRUE(calibration_from_json(j, c));
  TEST_ASSERT_EQUAL_DOUBLE(0, c.dist[0]);
}

void test_rejects_missing_or_malformed_fields() {
  Calibration c = {};
  TEST_ASSERT_FALSE(calibration_from_json("", c));
  TEST_ASSERT_FALSE(calibration_from_json("{}", c));
  // no cy
  TEST_ASSERT_FALSE(calibration_from_json(
      "{\"width\":800,\"height\":600,\"fx\":800,\"fy\":800,\"cx\":400}", c));
  // four distortion terms, six distortion terms, not a list
  const char *head = "{\"width\":800,\"height\":600,\"fx\":800,\"fy\":800,"
                     "\"cx\":400,\"cy\":300,";
  const char *tails[] = {"\"dist\":[0,0,0,0]}", "\"dist\":[0,0,0,0,0,0]}",
                         "\"dist\":0}", "\"dist\":[a,b,c,d,e]}"};
  for (const char *t : tails) {
    char buf[256];
    snprintf(buf, sizeof(buf), "%s%s", head, t);
    TEST_ASSERT_FALSE_MESSAGE(calibration_from_json(buf, c), t);
  }
  // fractional size
  TEST_ASSERT_FALSE(calibration_from_json(
      "{\"width\":800.5,\"height\":600,\"fx\":800,\"fy\":800,\"cx\":400,"
      "\"cy\":300}",
      c));
}

void test_a_rejected_body_leaves_the_output_alone() {
  Calibration c = {};
  c.fx = 123;
  TEST_ASSERT_FALSE(calibration_from_json("{\"width\":800}", c));
  TEST_ASSERT_EQUAL_DOUBLE(123, c.fx);
}

void test_validation_limits() {
  Calibration c = {800, 600, 800, 800, 400, 300, {0, 0, 0, 0, 0}};
  TEST_ASSERT_TRUE(calibration_valid(c));

  Calibration bad = c;
  bad.fx = 0; // "not calibrated" is DELETE, not a stored calibration
  TEST_ASSERT_FALSE(calibration_valid(bad));
  bad = c;
  bad.fy = 200; // fx/fy off by 4×
  TEST_ASSERT_FALSE(calibration_valid(bad));
  bad = c;
  bad.cx = 900; // outside the image
  TEST_ASSERT_FALSE(calibration_valid(bad));
  bad = c;
  bad.width = 0;
  TEST_ASSERT_FALSE(calibration_valid(bad));
  bad = c;
  bad.dist[1] = NAN;
  TEST_ASSERT_FALSE(calibration_valid(bad));
  bad = c;
  bad.dist[4] = 1e6;
  TEST_ASSERT_FALSE(calibration_valid(bad));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_parses_a_full_calibration);
  RUN_TEST(test_tolerates_whitespace_and_key_order);
  RUN_TEST(test_distortion_is_optional);
  RUN_TEST(test_rejects_missing_or_malformed_fields);
  RUN_TEST(test_a_rejected_body_leaves_the_output_alone);
  RUN_TEST(test_validation_limits);
  return UNITY_END();
}
