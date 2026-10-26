#pragma once
#include <freertos/FreeRTOS.h>
#include <stdint.h>

// Saving to the memory card, on its own task (writes take tens of ms, and the
// HTTP handlers must not wait): photos and clips asked for from the
// dashboard, timelapse photos, and the automatic ones when the camera is
// moved or the model sees a watched-for animal (options()).
//
// It takes frames from the camera like the stream does, so while it records
// a clip the live view gets about half the frames.

void recorder_start(BaseType_t core, UBaseType_t prio); // after storage_init
bool recorder_photo();            // queue one; false if no card is ready
bool recorder_record(bool start); // start (≤ rec_max_min) or stop a clip
bool recorder_recording();
int recorder_rec_s();       // length of the clip being recorded
const char *recorder_why(); // last thing that went wrong, "" if none
bool recorder_busy();       // writing: don't sleep now
// The timelapse photo, without the task: woken by the timer from sleep
bool recorder_timelapse_now();
// Seconds until the next timelapse photo is due, -1 when off
int32_t recorder_timelapse_in_s();
