// The QuickTime writer: every box's size adds up, and the index matches the
// frames that were written
#include "mov_writer.h"
#include <string.h>
#include <unity.h>
#include <vector>

void setUp() {}
void tearDown() {}

static uint32_t be32(const uint8_t *p) {
  return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3];
}

// Walks boxes in [p, p+len): each must fit exactly; containers recurse.
// Returns the number of boxes seen, -1 if any size is wrong.
static int walk(const uint8_t *p, size_t len, const uint8_t **found,
                const char *want) {
  static const char *CONTAINERS[] = {"moov", "trak", "mdia",
                                     "minf", "dinf", "stbl"};
  int n = 0;
  size_t at = 0;
  while (at < len) {
    if (len - at < 8)
      return -1;
    const uint32_t sz = be32(p + at);
    if (sz < 8 || sz > len - at)
      return -1;
    if (!memcmp(p + at + 4, want, 4))
      *found = p + at;
    n++;
    for (const char *c : CONTAINERS)
      if (!memcmp(p + at + 4, c, 4)) {
        const int k = walk(p + at + 8, sz - 8, found, want);
        if (k < 0)
          return -1;
        n += k;
      }
    at += sz;
  }
  return n;
}

void test_head_is_ftyp_then_mdat() {
  uint8_t h[MOV_HEAD];
  mov_head(h);
  TEST_ASSERT_EQUAL_UINT32(20, be32(h));
  TEST_ASSERT_EQUAL_MEMORY("ftyp", h + 4, 4);
  TEST_ASSERT_EQUAL_MEMORY("mdat", h + MOV_MDAT_SIZE_AT + 4, 4);
  uint8_t sz[4];
  mov_mdat_size(1000, sz);
  TEST_ASSERT_EQUAL_UINT32(1008, be32(sz));
}

void test_moov_boxes_add_up_and_index_frames() {
  std::vector<MovFrame> f = {
      {28, 1000, 66}, {1028, 1200, 66}, {2228, 900, 67}, {3128, 1100, 67}};
  std::vector<uint8_t> buf(mov_moov_bound(f.size()));
  const size_t len =
      mov_moov(buf.data(), buf.size(), f.data(), f.size(), 320, 240, 0);
  TEST_ASSERT_GREATER_THAN(0, len);
  const uint8_t *stsz = nullptr;
  TEST_ASSERT_GREATER_THAN(10, walk(buf.data(), len, &stsz, "stsz"));
  TEST_ASSERT_NOT_NULL(stsz);
  TEST_ASSERT_EQUAL_UINT32(4, be32(stsz + 16)); // sample count
  TEST_ASSERT_EQUAL_UINT32(1200, be32(stsz + 24));
  const uint8_t *stco = nullptr;
  walk(buf.data(), len, &stco, "stco");
  TEST_ASSERT_EQUAL_UINT32(2228, be32(stco + 16 + 8)); // offsets[2]
  const uint8_t *stts = nullptr;
  walk(buf.data(), len, &stts, "stts");
  TEST_ASSERT_EQUAL_UINT32(2, be32(stts + 12)); // two runs: 66 ms, 67 ms
  const uint8_t *mvhd = nullptr;
  walk(buf.data(), len, &mvhd, "mvhd");
  TEST_ASSERT_EQUAL_UINT32(266, be32(mvhd + 8 + 16)); // duration, ms
}

void test_too_small_buffer_is_refused() {
  MovFrame f = {28, 10, 50};
  uint8_t small[64];
  TEST_ASSERT_EQUAL(0, mov_moov(small, sizeof(small), &f, 1, 8, 8, 0));
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_head_is_ftyp_then_mdat);
  RUN_TEST(test_moov_boxes_add_up_and_index_frames);
  RUN_TEST(test_too_small_buffer_is_refused);
  return UNITY_END();
}
