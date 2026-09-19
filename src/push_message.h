#pragma once
#include <stddef.h>
#include <string.h>

// Subscription datagrams for the UDP push: "sub <token>" subscribes or
// refreshes, "bye <token>" unsubscribes. The token is API_TOKEN, so a spoofed
// source address can neither aim the stream at a victim nor cancel someone
// else's subscription. Header-only, no Arduino: host-tested.
enum class PushCmd { None, Sub, Bye };

inline PushCmd push_parse(const char *msg, size_t len, const char *token) {
  if (!token[0] || len < 5 || msg[3] != ' ') // empty token: push is disabled
    return PushCmd::None;
  const size_t tlen = strlen(token);
  if (len - 4 != tlen)
    return PushCmd::None;
  unsigned char diff = 0; // constant time in the token
  for (size_t i = 0; i < tlen; i++)
    diff |= (unsigned char)(msg[4 + i] ^ token[i]);
  if (diff)
    return PushCmd::None;
  if (!memcmp(msg, "sub", 3))
    return PushCmd::Sub;
  if (!memcmp(msg, "bye", 3))
    return PushCmd::Bye;
  return PushCmd::None;
}
