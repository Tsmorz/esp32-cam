#pragma once
#include <esp_camera.h>

// Which output size the sensor is at, and who may touch it when.
//
// The sensor has one output size, shared by the MJPEG stream and the detector,
// and detection speed depends on it. The size picked for the stream applies
// while anyone watches the stream or takes a snapshot; when nobody does, the
// sensor drops to the detection size if one is set (and the detector runs).
// Going back to the stream size is immediate, going back to the detection
// size waits a few seconds after the last viewer so a burst of snapshots
// doesn't flip the sensor back and forth.
//
// Every write to sensor registers (size, mirror, quality, autofocus...) takes
// the sensor lock: set_framesize() rewrites many registers, and the setters
// read-modify-write the same ones.

void camera_profile_init(); // before esp_camera_init()

// Recursive, so a setter may call back into camera_profile_update()
void sensor_lock();
void sensor_unlock();

bool camera_valid_framesize(int framesize); // the sizes the dashboard offers

int camera_stream_size(); // framesize_t the user picked for the stream
bool camera_set_stream_size(int framesize);
int camera_detect_size(); // framesize_t; 0 = detect at the stream size
bool camera_set_detect_size(int framesize, bool persist = true);

int camera_stream_viewers();
void camera_viewer_change(int delta); // stream connect (+1) / disconnect (-1)
// Move the sensor to the size that fits the current viewers. Cheap when
// nothing changed: call it every so often (loop()) and after any change.
void camera_profile_update();

// A frame at the stream size (the sensor is moved there first, waiting for it);
// return it with camera_fb_return_at_stream_size(). For /snapshot.
camera_fb_t *camera_fb_get_at_stream_size();
void camera_fb_return_at_stream_size(camera_fb_t *fb);
