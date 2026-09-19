#pragma once
#include "detection_types.h"
#include <esp_camera.h>
#include <freertos/FreeRTOS.h>
#include <stddef.h>
#include <stdint.h>

// TAG_SIZE_M: edge of the printed black square in metres (not the white
// border around it). /tag prints at exactly this size.
// Pose accuracy scales linearly with this — measure your tag and update.
static constexpr double TAG_SIZE_M = 0.165;

bool detector_init();  // allocate buffers + apriltag detector, self-test
bool detector_ready(); // detector_init() succeeded
void detector_start(BaseType_t core, UBaseType_t prio, uint32_t period_ms);
// Hand a frame the stream is about to send to the detector, if it is waiting
// for one. While a stream runs the detector takes its frames from here.
void detector_offer(const camera_fb_t *fb);
void detector_toggle();
void detector_set_enabled(bool on);
bool detector_enabled();
DetectionFrame detector_latest(); // thread-safe copy
// Last grayscale image the detector ran on, as an 8-bit BMP (caller frees)
size_t detector_gray_bmp(uint8_t **out);
// Printable tag `id` as SVG, black square TAG_SIZE_M wide; 0 if id invalid
size_t detector_tag_svg(int id, char *buf, size_t size);
