#pragma once
#include <freertos/FreeRTOS.h>
#include <stddef.h>
#include <stdint.h>

// Animal detection with a TensorFlow Lite model uploaded from the dashboard
// (e.g. an Edge Impulse export: a classifier, or a FOMO model that also says
// where). The model and its labels are kept in the flash partition the
// firmware doesn't otherwise use ("spiffs", 3.4 MB), copied to PSRAM and run
// with TensorFlow Lite Micro + esp-nn on core 1, about once a second.
//
// When options().detect_on is set, the newest frame is decoded at 1/2–1/8
// size, centre-cropped to the model's shape and run; a watched-for label
// above options().detect_thr raises a trigger for the recorder.

void detect_start(BaseType_t core, UBaseType_t prio);
// "none" (no model), "off", "loading", "running" or "error"
const char *detect_state();
const char *detect_model_name();
// Model, input, timing and the latest results (private: the video's rules)
size_t detect_json(char *buf, size_t size);
bool detect_take_trigger(); // a watched-for animal since the last call

// Upload, from the HTTP handler: begin() stops detection and checks the
// size, write() streams (model bytes, then the labels text, one per line),
// end() stores the header that makes it valid and reloads.
// range: InputRange (0: pixels 0..1, 1: -1..1, 2: 0..255).
bool detect_upload_begin(const char *name, size_t model_len, size_t labels_len,
                         int range, const char **why);
bool detect_upload_write(const uint8_t *data, size_t n);
bool detect_upload_end(const char **why);
void detect_forget(); // erase the stored model
