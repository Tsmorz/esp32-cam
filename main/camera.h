#pragma once
#include <esp_camera.h>
#include <esp_http_server.h>

bool camera_init();
const char *camera_sensor_name(); // e.g. "OV5640"; "none" before init
// false = unknown var or out of range. persist: remember across reboots.
// hmirror and rotate (quarter turns clockwise) are how the viewer wants the
// picture shown, not sensor registers: see view.h
bool camera_set(const char *var, int val, bool persist = true);
// Straighten: "x0,y0,…,x3,y3" in thousandths of the picture as shown, or ""
// for none (view.h). False = not four corners in order round the edge.
bool camera_set_quad(const char *corners);
void camera_restore();        // re-apply saved settings (after focus_init)
void camera_reset_settings(); // forget saved settings (takes effect on boot)
size_t camera_status_json(char *buf, size_t size);
esp_err_t stream_handler(httpd_req_t *req);
// Stream size, viewers, the sensor lock: camera_profile.h
