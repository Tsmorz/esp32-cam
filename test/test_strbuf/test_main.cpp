// The JSON helpers the HTTP handlers build and parse with
#include "strbuf.h"
#include <string.h>
#include <unity.h>

void setUp() {}
void tearDown() {}

void test_json_str_reads_values() {
  char out[33];
  const char *body = "{\"ssid\":\"Home \\\"5G\\\"\",\"pass\":\"a\\\\b\"}";
  TEST_ASSERT_TRUE(json_str(body, "ssid", out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("Home \"5G\"", out);
  TEST_ASSERT_TRUE(json_str(body, "pass", out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("a\\b", out);
  TEST_ASSERT_FALSE(json_str(body, "nope", out, sizeof(out)));
}

void test_json_str_ignores_keys_inside_values() {
  // An SSID that looks like a key, escaped as the dashboard sends it
  char out[33];
  const char *body =
      "{\"ssid\":\"x\\\",\\\"pass\\\":\\\"evil\",\"pass\":\"real\"}";
  TEST_ASSERT_TRUE(json_str(body, "pass", out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("real", out);
}

void test_json_str_truncates() {
  char out[5];
  TEST_ASSERT_TRUE(json_str("{\"k\":\"abcdefgh\"}", "k", out, sizeof(out)));
  TEST_ASSERT_EQUAL_STRING("abcd", out);
}

void test_json_esc_escapes_and_drops_control() {
  char out[16];
  json_esc("a\"b\\c\nd", out, sizeof(out));
  TEST_ASSERT_EQUAL_STRING("a\\\"b\\\\cd", out);
  json_esc("\"\"\"\"\"\"\"\"\"\"", out, 8); // never splits an escape
  TEST_ASSERT_EQUAL_STRING("\\\"\\\"\\\"", out);
}

void test_appendf_truncates_without_overrun() {
  char buf[8];
  memset(buf, 'X', sizeof(buf));
  size_t pos = 0;
  appendf(buf, sizeof(buf), pos, "%s", "abc");
  appendf(buf, sizeof(buf), pos, "%d", 12345);
  TEST_ASSERT_EQUAL_UINT(7, pos);
  TEST_ASSERT_EQUAL_STRING("abc1234", buf);
  appendf(buf, sizeof(buf), pos, "more"); // full: nothing happens
  TEST_ASSERT_EQUAL_UINT(7, pos);
}

void test_url_decode() {
  char s[] = "cat%2C+dog%20%41%zz";
  url_decode(s);
  TEST_ASSERT_EQUAL_STRING("cat, dog A%zz", s);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_json_str_reads_values);
  RUN_TEST(test_json_str_ignores_keys_inside_values);
  RUN_TEST(test_json_str_truncates);
  RUN_TEST(test_json_esc_escapes_and_drops_control);
  RUN_TEST(test_appendf_truncates_without_overrun);
  RUN_TEST(test_url_decode);
  return UNITY_END();
}
