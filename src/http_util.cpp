#include "http_util.h"
#include "config.h"
#include <cstdio>
#include <cstring>

// Being a custom header, X-Token also forces a CORS preflight, which is never
// answered, so other websites can't fire these routes from a visitor's browser.
bool authorized(httpd_req_t *req) {
  char tok[65] = {};
  if (API_TOKEN[0] &&
      httpd_req_get_hdr_value_str(req, "X-Token", tok, sizeof(tok)) == ESP_OK &&
      strcmp(tok, API_TOKEN) == 0)
    return true;
  httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED,
                      API_TOKEN[0] ? "Missing or wrong X-Token"
                                   : "API_TOKEN not set in secrets.h");
  return false;
}

bool json_str(const char *json, const char *key, char *out, size_t outlen) {
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

bool read_body(httpd_req_t *req, char *buf, size_t size) {
  if (req->content_len == 0 || req->content_len >= size) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body missing or too big");
    return false;
  }
  size_t got = 0;
  while (got < req->content_len) {
    const int r = httpd_req_recv(req, buf + got, req->content_len - got);
    if (r <= 0)
      return false;
    got += r;
  }
  buf[got] = '\0';
  return true;
}

esp_err_t send_json(httpd_req_t *req, const char *buf, size_t len) {
  httpd_resp_set_type(req, "application/json");
  return httpd_resp_send(req, buf, len);
}
