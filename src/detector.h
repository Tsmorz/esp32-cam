#pragma once
#include <stdint.h>
#include <stddef.h>

static constexpr int MAX_TAGS = 8;

// TAG_SIZE_M: physical side length of the printed tag in metres.
// Pose accuracy scales linearly with this — measure your tag and update.
static constexpr double TAG_SIZE_M = 0.165;

struct TagDetection {
    int   id;
    float corners[4][2];  // [corner_idx][x/y] in full-res (800×600) pixels
    double tx, ty, tz;   // translation from camera to tag centre, metres
};

struct DetectionFrame {
    int           count;
    TagDetection  tags[MAX_TAGS];
};

void                  detector_init();
void                  detector_grab();          // call from loop() on a timer
void                  detector_toggle();
const DetectionFrame& detector_latest();
