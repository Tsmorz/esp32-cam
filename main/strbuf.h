#pragma once

// Small string helpers for building JSON into fixed buffers. No Arduino, so
// they are usable from host-side tests too.

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

// Copy `in` to `out` escaped for a JSON string (quotes, backslashes; control
// characters dropped)
static inline void json_esc(const char *in, char *out, size_t outlen) {
  size_t o = 0;
  for (const char *p = in; *p && o < outlen - 2; p++) {
    if (*p == '"' || *p == '\\')
      out[o++] = '\\';
    if ((unsigned char)*p >= 0x20)
      out[o++] = *p;
  }
  out[o] = '\0';
}

// snprintf that appends at pos and truncates instead of running off the end
static inline void __attribute__((format(printf, 4, 5)))
appendf(char *buf, size_t size, size_t &pos, const char *fmt, ...) {
  if (pos >= size - 1)
    return;
  va_list ap;
  va_start(ap, fmt);
  const int n = vsnprintf(buf + pos, size - pos, fmt, ap);
  va_end(ap);
  if (n > 0)
    pos = (pos + n < size) ? pos + n : size - 1;
}

// Value of a flat "key":"string" pair in a JSON object (\" and \\ unescaped,
// truncated to fit); false if absent
static inline bool json_str(const char *json, const char *key, char *out,
                            size_t outlen) {
  char needle[64];
  snprintf(needle, sizeof(needle), "\"%s\":\"", key);
  const char *p = strstr(json, needle);
  if (!p)
    return false;
  p += strlen(needle);
  size_t i = 0;
  while (*p && *p != '"' && i < outlen - 1) {
    if (*p == '\\' && *(p + 1))
      p++;
    out[i++] = *p++;
  }
  out[i] = '\0';
  return true;
}

// Decode a query-string value in place (%XX and '+'); the httpd hands them
// over still encoded
static inline void url_decode(char *s) {
  char *o = s;
  for (; *s; s++) {
    const auto hex = [](char c) {
      return c >= '0' && c <= '9'   ? c - '0'
             : c >= 'a' && c <= 'f' ? c - 'a' + 10
             : c >= 'A' && c <= 'F' ? c - 'A' + 10
                                    : -1;
    };
    if (*s == '+') {
      *o++ = ' ';
    } else if (*s == '%' && hex(s[1]) >= 0 && hex(s[2]) >= 0) {
      *o++ = (char)(hex(s[1]) * 16 + hex(s[2]));
      s += 2;
    } else {
      *o++ = *s;
    }
  }
  *o = '\0';
}
