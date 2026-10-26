#include "token_rules.h"
#include <string>
#include <unity.h>

void setUp() {}
void tearDown() {}

void test_accepts_reasonable_tokens() {
  TEST_ASSERT_TRUE(token_valid("abcdefgh"));
  TEST_ASSERT_TRUE(token_valid("a1b2-c3d4_E5F6.g7"));
  TEST_ASSERT_TRUE(token_valid(std::string(63, 'x').c_str()));
}

void test_rejects_bad_length() {
  TEST_ASSERT_FALSE(token_valid(""));
  TEST_ASSERT_FALSE(token_valid("short12"));
  TEST_ASSERT_FALSE(token_valid(std::string(64, 'x').c_str()));
}

void test_rejects_spaces_and_control_and_non_ascii() {
  TEST_ASSERT_FALSE(token_valid("has space here"));
  TEST_ASSERT_FALSE(token_valid("tab\there1234"));
  TEST_ASSERT_FALSE(token_valid("new\nline1234"));
  TEST_ASSERT_FALSE(token_valid("caf\xc3\xa9token"));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_accepts_reasonable_tokens);
  RUN_TEST(test_rejects_bad_length);
  RUN_TEST(test_rejects_spaces_and_control_and_non_ascii);
  return UNITY_END();
}
