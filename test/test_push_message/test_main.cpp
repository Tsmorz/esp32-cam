#include "push_message.h"
#include <unity.h>

static PushCmd parse(const char *m, const char *tok = "secret") {
  return push_parse(m, strlen(m), tok);
}

void setUp() {}
void tearDown() {}

void test_sub_and_bye() {
  TEST_ASSERT_TRUE(parse("sub secret") == PushCmd::Sub);
  TEST_ASSERT_TRUE(parse("bye secret") == PushCmd::Bye);
}

void test_rejects_bad_token() {
  TEST_ASSERT_TRUE(parse("sub wrong!") == PushCmd::None);
  TEST_ASSERT_TRUE(parse("sub secre") == PushCmd::None);
  TEST_ASSERT_TRUE(parse("sub secrets") == PushCmd::None);
  TEST_ASSERT_TRUE(parse("bye") == PushCmd::None);
  TEST_ASSERT_TRUE(parse("hi") == PushCmd::None);
  TEST_ASSERT_TRUE(parse("") == PushCmd::None);
}

void test_rejects_unknown_verb() {
  TEST_ASSERT_TRUE(parse("foo secret") == PushCmd::None);
  TEST_ASSERT_TRUE(parse("subsecret") == PushCmd::None);
}

void test_disabled_without_token() {
  TEST_ASSERT_TRUE(parse("sub ", "") == PushCmd::None);
  TEST_ASSERT_TRUE(parse("sub x", "") == PushCmd::None);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_sub_and_bye);
  RUN_TEST(test_rejects_bad_token);
  RUN_TEST(test_rejects_unknown_verb);
  RUN_TEST(test_disabled_without_token);
  return UNITY_END();
}
