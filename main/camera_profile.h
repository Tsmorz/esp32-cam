#pragma once
#include <esp_camera.h>

// The sensor's output size, who is watching, and who may touch the sensor
// when.
//
// Every write to sensor registers (size, mirror, quality, autofocus...) takes
// the sensor lock: set_framesize() rewrites many registers, and the setters
// read-modify-write the same ones.

void camera_profile_init(); // before esp_camera_init()

// Recursive, so a setter may call another setter
void sensor_lock();
void sensor_unlock();

bool camera_valid_framesize(int framesize); // the sizes the dashboard offers

int camera_stream_size(); // framesize_t the user picked
bool camera_set_stream_size(int framesize);

// Open MJPEG stream connections (the stream server takes one at a time)
int camera_stream_viewers();
void camera_viewer_change(int delta); // stream connect (+1) / disconnect (-1)

// A frame at the stream size, skipping any still queued at the size before a
// change; return it with camera_fb_return_at_stream_size(). For /snapshot.
camera_fb_t *camera_fb_get_at_stream_size();
void camera_fb_return_at_stream_size(camera_fb_t *fb);
