#pragma once
#include <esp_http_server.h>
#include <stddef.h>

// State-changing routes require X-Token == the API token (auth.h); sends the
// 401 itself
bool authorized(httpd_req_t *req);
bool token_ok(httpd_req_t *req); // the same check, sends nothing
// Video and motion data (/stream, /snapshot, /pose): only over the home
// network — the hotspot is for setup — and only with the password (X-Token,
// or the cookie GET /auth sets). Sends the 403/401 itself.
bool video_allowed(httpd_req_t *req);

// The camera's address on the interface this request came in on; false if
// unknown. out: at least 16 bytes.
bool local_ip(httpd_req_t *req, char *out);
bool via_hotspot(httpd_req_t *req);
// The Host header names the camera (one of its addresses, HOSTNAME.local, or
// none). Anything else is a phone's connectivity check (captive portal) or a
// DNS-rebinding page, and gets redirect_home().
bool host_ok(httpd_req_t *req);
esp_err_t redirect_home(httpd_req_t *req); // 302 to http://<local_ip>/

// Read the whole request body (NUL-terminated); false if empty or too big
bool read_body(httpd_req_t *req, char *buf, size_t size);
esp_err_t send_json(httpd_req_t *req, const char *buf, size_t len);
