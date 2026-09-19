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

// ── Stream size vs. detection size ──
// Detection speed depends on the sensor's output size, which the MJPEG stream
// shares. The size picked for the stream (camera_set "framesize") applies while
// anyone watches the stream or takes a snapshot; when nobody does, the sensor
// drops to the detection size if one is set.
int camera_detect_size(); // framesize_t; 0 = detect at the stream size
bool camera_set_detect_size(int framesize, bool persist = true);
int camera_stream_viewers();
// Move the sensor to the size that fits the current viewers. Cheap when
// nothing changed: call it every so often (loop()) and after any change.
void camera_profile_update();
// A frame at the stream size (the sensor is moved there first, waiting for it);
// return it with camera_fb_return_at_stream_size(). For /snapshot.
camera_fb_t *camera_fb_get_at_stream_size();
void camera_fb_return_at_stream_size(camera_fb_t *fb);
