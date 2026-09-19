#pragma once
#include <esp_http_server.h>
#include <stddef.h>

// State-changing routes require X-Token == the API token (auth.h); sends the
// 401 itself
bool authorized(httpd_req_t *req);
// Value of a flat "key":"string" pair; false if absent
bool json_str(const char *json, const char *key, char *out, size_t outlen);
// Read the whole request body (NUL-terminated); false if empty or too big
bool read_body(httpd_req_t *req, char *buf, size_t size);
esp_err_t send_json(httpd_req_t *req, const char *buf, size_t len);
