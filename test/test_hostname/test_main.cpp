// The camera's network name: what http://<name>.local accepts
#include "hostname_rules.h"
#include <unity.h>

void setUp() {}
void tearDown() {}

void test_accepts() {
  TEST_ASSERT_TRUE(hostname_valid("wifi-camera"));
  TEST_ASSERT_TRUE(hostname_valid("a"));
  TEST_ASSERT_TRUE(hostname_valid("garden-cam-2"));
  TEST_ASSERT_TRUE(hostname_valid("2nd-floor"));
  TEST_ASSERT_TRUE(hostname_valid("abcdefghijklmnopqrstuvwxyz012345")); // 32
}

void test_refuses() {
  const char *bad[] = {
      "",                                  // empty
      "abcdefghijklmnopqrstuvwxyz0123456", // 33
      "-cam",
      "cam-",   // dash at an end
      "Garden", // uppercase: the dashboard lowers it
      "garden cam",
      "garden_cam", // space, underscore
      "cam.local",  // a dot: .local is added
      "café",       // not ASCII
  };
  for (const char *h : bad)
    TEST_ASSERT_FALSE_MESSAGE(hostname_valid(h), h);
}

// config.h's HOSTNAME is static_asserted in wifi_mgr.cpp
void test_built_in_name_is_valid() {
  TEST_ASSERT_TRUE(hostname_valid("wifi-camera"));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_accepts);
  RUN_TEST(test_refuses);
  RUN_TEST(test_built_in_name_is_valid);
  return UNITY_END();
}
