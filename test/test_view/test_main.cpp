// Turning, mirroring and straightening the picture: the sensor's part plus the
// dashboard's quarter turn give what the viewer asked for, and the corners of
// a straightened wall follow the picture when it's turned or mirrored later
#include "view.h"
#include <string.h>
#include <unity.h>

using namespace view;

void setUp() {}
void tearDown() {}

static bool same(Sym p, Sym q) {
  return p.a == q.a && p.b == q.b && p.c == q.c && p.d == q.d;
}

static Quad quad(const char *s) {
  Quad q;
  TEST_ASSERT_TRUE_MESSAGE(parse(s, q), s);
  return q;
}

void test_turn_is_clockwise() {
  // The top edge's middle (0, -1) goes to the right edge's middle (1, 0)
  const Sym t = turned(1);
  TEST_ASSERT_EQUAL(1, t.a * 0 + t.b * -1);
  TEST_ASSERT_EQUAL(0, t.c * 0 + t.d * -1);
  TEST_ASSERT_TRUE(same(turned(4), IDENTITY));
  TEST_ASSERT_TRUE(same(turned(-1), turned(3)));
}

void test_split_gives_what_was_asked() {
  for (int turns = 0; turns < 4; turns++)
    for (int mirror = 0; mirror < 2; mirror++) {
      const Split s = split(turns, mirror);
      const Sym sensor = {s.hmirror ? -1 : 1, 0, 0, s.vflip ? -1 : 1};
      TEST_ASSERT_TRUE(
          same(mul(turned(s.quarter), sensor), on_screen(turns, mirror)));
    }
}

void test_split_known_cases() {
  Split s = split(0, false);
  TEST_ASSERT_FALSE(s.hmirror || s.vflip || s.quarter);
  s = split(0, true); // mirror only: the sensor does it
  TEST_ASSERT_TRUE(s.hmirror && !s.vflip && !s.quarter);
  s = split(2, false); // upside down: the sensor does both
  TEST_ASSERT_TRUE(s.hmirror && s.vflip && !s.quarter);
  s = split(1, false); // sideways: all in the dashboard
  TEST_ASSERT_TRUE(!s.hmirror && !s.vflip && s.quarter == 1);
  s = split(1, true); // sideways and mirrored: a flip, then the quarter turn
  TEST_ASSERT_TRUE(!s.hmirror && s.vflip && s.quarter == 1);
  s = split(3, false);
  TEST_ASSERT_TRUE(s.hmirror && s.vflip && s.quarter == 1);
}

void test_parse_and_format() {
  Quad q = quad("100,150,900,100,950,900,50,850");
  TEST_ASSERT_TRUE(q.set);
  TEST_ASSERT_EQUAL(950, q.p[4]);
  char buf[48];
  format(q, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("100,150,900,100,950,900,50,850", buf);
  q = quad("");
  TEST_ASSERT_FALSE(q.set);
  format(q, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("", buf);
}

void test_parse_rejects() {
  const char *bad[] = {
      "100,100,900,100,900,900",           // too few
      "100,100,900,100,900,900,100,900,5", // too many
      "100,100,900,100,900,900,100,",      // empty last
      "100,100,900,100,900,900,100,x",     // not a number
      "-1,100,900,100,900,900,100,900",    // outside the frame
      "100,100,1001,100,900,900,100,900",  // outside the frame
      "100,100,100,900,900,900,900,100",   // anticlockwise
      "100,100,900,900,900,100,100,900",   // crossed
      "100,100,900,100,500,200,100,900",   // dented
      "480,480,520,480,520,520,480,520",   // too small (0.16 %)
      "100,100,900,100,900,100,100,100",   // flat
  };
  for (const char *s : bad) {
    Quad q = quad("200,200,800,200,800,800,200,800");
    TEST_ASSERT_FALSE_MESSAGE(parse(s, q), s);
    TEST_ASSERT_EQUAL_MESSAGE(200, q.p[0], "left alone on failure");
  }
}

void test_whole_frame_is_valid() {
  TEST_ASSERT_TRUE(quad("0,0,1000,0,1000,1000,0,1000").set);
}

void test_small_picture_frame_is_valid() {
  // A frame across the room: 15 % × 15 % of the picture, seen at an angle
  TEST_ASSERT_TRUE(quad("600,300,750,320,745,470,598,455").set);
  TEST_ASSERT_TRUE(quad("475,475,525,475,525,525,475,525").set); // 0.25 %
}

void test_moved_by_a_turn() {
  // (x, y) → (1000 − y, x): the top left corner (100,150) becomes the top
  // right (850,100), the bottom left (50,850) the top left (150,50)
  const Quad q = moved(quad("100,150,900,100,950,900,50,850"), turned(1));
  char buf[48];
  format(q, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("150,50,850,100,900,900,100,950", buf);
  TEST_ASSERT_TRUE(valid(q));
}

void test_moved_by_a_mirror() {
  const Quad q = moved(quad("100,150,900,100,950,900,50,850"), MIRROR);
  char buf[48];
  format(q, buf, sizeof(buf));
  TEST_ASSERT_EQUAL_STRING("100,100,900,150,950,850,50,900", buf);
  TEST_ASSERT_TRUE(valid(q));
}

void test_moved_round_trip() {
  const Quad q = quad("100,150,900,100,950,900,50,850");
  for (int turns = 0; turns < 4; turns++)
    for (int mirror = 0; mirror < 2; mirror++) {
      const Sym s = on_screen(turns, mirror);
      const Quad m = moved(q, s);
      TEST_ASSERT_TRUE(valid(m));
      const Quad back = moved(m, inverse(s));
      TEST_ASSERT_EQUAL_INT_ARRAY(q.p, back.p, 8);
    }
}

void test_moved_unset_stays_unset() {
  TEST_ASSERT_FALSE(moved(Quad(), turned(1)).set);
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_turn_is_clockwise);
  RUN_TEST(test_split_gives_what_was_asked);
  RUN_TEST(test_split_known_cases);
  RUN_TEST(test_parse_and_format);
  RUN_TEST(test_parse_rejects);
  RUN_TEST(test_whole_frame_is_valid);
  RUN_TEST(test_small_picture_frame_is_valid);
  RUN_TEST(test_moved_by_a_turn);
  RUN_TEST(test_moved_by_a_mirror);
  RUN_TEST(test_moved_round_trip);
  RUN_TEST(test_moved_unset_stays_unset);
  return UNITY_END();
}
