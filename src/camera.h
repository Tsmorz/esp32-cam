#pragma once
#include <esp_http_server.h>

bool camera_init();
esp_err_t index_handler(httpd_req_t *req);
esp_err_t stream_handler(httpd_req_t *req);
