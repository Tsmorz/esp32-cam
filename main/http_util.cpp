#include "http_util.h"
#include "auth.h"
#include "config.h"
#include "hostname_rules.h"
#include "wifi_mgr.h"
#include <WiFi.h>
#include <cstdio>
#include <cstring>
#include <lwip/sockets.h>
#include <strings.h>

// Being a custom header, X-Token also forces a CORS preflight, which is never
// answered, so other websites can't fire these routes from a visitor's browser.
bool token_ok(httpd_req_t *req) {
  char tok[65] = {};
  return auth_configured() &&
         httpd_req_get_hdr_value_str(req, "X-Token", tok, sizeof(tok)) ==
             ESP_OK &&
         strcmp(tok, auth_token()) == 0;
}

bool authorized(httpd_req_t *req) {
  if (token_ok(req))
    return true;
  httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED,
                      auth_configured()
                          ? "Missing or wrong X-Token"
                          : "No API token yet — claim one in the dashboard");
  return false;
}

bool video_allowed(httpd_req_t *req) {
  if (via_hotspot(req)) {
    httpd_resp_send_err(req, HTTPD_403_FORBIDDEN,
                        "No video on the camera's own WiFi: it is for setup. "
                        "Open the camera on your home WiFi.");
    return false;
  }
  char c[72] = {};
  size_t n = sizeof(c);
  if (token_ok(req) ||
      (auth_configured() &&
       httpd_req_get_cookie_val(req, AUTH_COOKIE, c, &n) == ESP_OK &&
       strcmp(c, auth_video_cookie()) == 0))
    return true;
  httpd_resp_send_err(req, HTTPD_401_UNAUTHORIZED, "Password needed");
  return false;
}

bool local_ip(httpd_req_t *req, char *out) {
  out[0] = '\0';
  struct sockaddr_storage sa;
  socklen_t len = sizeof(sa);
  if (getsockname(httpd_req_to_sockfd(req), (struct sockaddr *)&sa, &len))
    return false;
  if (sa.ss_family == AF_INET)
    inet_ntop(AF_INET, &((struct sockaddr_in *)&sa)->sin_addr, out,
              INET_ADDRSTRLEN);
  else if (sa.ss_family == AF_INET6) // IPv4-mapped: the last 4 bytes
    inet_ntop(AF_INET, &((struct sockaddr_in6 *)&sa)->sin6_addr.un.u32_addr[3],
              out, INET_ADDRSTRLEN);
  return out[0] != '\0';
}

bool via_hotspot(httpd_req_t *req) {
  char self[INET_ADDRSTRLEN];
  return (WiFi.getMode() & WIFI_AP) && local_ip(req, self) &&
         !strcmp(self, WiFi.softAPIP().toString().c_str());
}

bool host_ok(httpd_req_t *req) {
  char host[64] = "";
  httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host));
  char *colon = strchr(host, ':');
  if (colon)
    *colon = '\0';
  const size_t n = strlen(host);
  if (n && host[n - 1] == '.') // fully qualified: wifi-camera.local.
    host[n - 1] = '\0';
  char self[INET_ADDRSTRLEN], name[HOSTNAME_MAX + 8];
  wifi_hostname(name, sizeof(name));
  strlcat(name, ".local", sizeof(name));
  return !host[0] || (local_ip(req, self) && !strcmp(host, self)) ||
         !strcasecmp(host, name) ||
         !strcmp(host, WiFi.softAPIP().toString().c_str()) ||
         !strcmp(host, WiFi.localIP().toString().c_str());
}

esp_err_t redirect_home(httpd_req_t *req) {
  char self[INET_ADDRSTRLEN];
  if (!local_ip(req, self)) {
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
    return ESP_FAIL;
  }
  char loc[48];
  snprintf(loc, sizeof(loc), "http://%s/", self);
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", loc);
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_send(req, nullptr, 0);
  return ESP_OK;
}

bool read_body(httpd_req_t *req, char *buf, size_t size) {
  if (req->content_len == 0 || req->content_len >= size) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Body missing or too big");
    return false;
  }
  size_t got = 0;
  int timeouts = 0;
  while (got < req->content_len) {
    const int r = httpd_req_recv(req, buf + got, req->content_len - got);
    if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 3)
      continue; // recv_wait_timeout: the client is slow, not gone
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
