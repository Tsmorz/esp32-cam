#pragma once
#include <esp_http_server.h>

bool camera_init();
void camera_set(const char *var, int val);
esp_err_t index_handler(httpd_req_t *req);
esp_err_t stream_handler(httpd_req_t *req);
