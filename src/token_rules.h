#pragma once
#include <stddef.h>

// What may be used as the API token: 8–63 printable ASCII characters, no
// spaces (it travels in an HTTP header, and doubles as the hotspot's WPA2
// passphrase, which is at most 63). Header-only, no Arduino: host-tested.
static constexpr size_t TOKEN_MIN = 8, TOKEN_MAX = 63;

static constexpr bool token_valid(const char *t) {
  size_t n = 0;
  for (; t[n]; n++)
    if (t[n] < 0x21 || t[n] > 0x7e)
      return false;
  return n >= TOKEN_MIN && n <= TOKEN_MAX;
}
