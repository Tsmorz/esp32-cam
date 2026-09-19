#pragma once
#include <esp_camera.h>
#include <esp_http_server.h>

bool camera_init();
const char *camera_sensor_name(); // e.g. "OV5640"; "none" before init
// false = unknown var or out of range. persist: remember across reboots.
bool camera_set(const char *var, int val, bool persist = true);
void camera_restore();        // re-apply saved settings (after focus_init)
void camera_reset_settings(); // forget saved settings (takes effect on boot)
size_t camera_status_json(char *buf, size_t size);
esp_err_t stream_handler(httpd_req_t *req);
// Called with every frame the MJPEG stream sends, before it is sent
void camera_on_stream_frame(void (*cb)(const camera_fb_t *fb));
// Stream vs. detection size, viewers, the sensor lock: camera_profile.h
