#pragma once

// Rollback for a bad OTA image. The bootloader boots a fresh OTA image on
// probation: if it resets before being marked valid, the previous image is
// booted instead. Arduino marks it valid before setup() runs, which would
// defeat that, so ota_health.cpp holds it back until the firmware has proved
// itself: still up, with the HTTP servers running, OTA_CONFIRM_MS after boot.
static constexpr unsigned OTA_CONFIRM_MS = 20000;

void ota_health_tick(); // call from loop(); marks the image valid once healthy
bool ota_health_pending(); // still on probation
