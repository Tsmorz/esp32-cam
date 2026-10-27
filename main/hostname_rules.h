#pragma once
#include <stddef.h>

// The camera's name on the network: http://<name>.local (mDNS) and the name
// the router lists. One DNS label, lowercase: letters, digits and dashes, not
// at either end; at most 32 (the DHCP hostname's limit). Header-only, no
// Arduino: host-tested.
static constexpr size_t HOSTNAME_MAX = 32;

static constexpr bool hostname_valid(const char *h) {
  size_t n = 0;
  for (; h[n]; n++) {
    const char c = h[n];
    if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'))
      return false;
  }
  return n >= 1 && n <= HOSTNAME_MAX && h[0] != '-' && h[n - 1] != '-';
}
