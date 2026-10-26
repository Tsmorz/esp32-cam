#pragma once

// Rollback for a bad OTA image. The bootloader boots a fresh OTA image on
// probation: if it resets before being marked valid, the previous image is
// booted instead. Arduino marks it valid before setup() runs, which would
// defeat that, so ota_health.cpp holds it back until the firmware has proved
// itself: OTA_CONFIRM_MS after setup() finished, with the HTTP servers up and
// a network to reach them on. An image that never gets there is rolled back
// after OTA_GIVE_UP_MS.
static constexpr unsigned OTA_CONFIRM_MS = 20000;
static constexpr unsigned OTA_GIVE_UP_MS = 120000;

void ota_health_init();  // early in setup(): is this image on probation?
void ota_health_start(); // end of setup(): the probation clock starts
// call from loop(); healthy = the HTTP servers run and STA or the AP is up
void ota_health_tick(bool healthy);
bool ota_health_pending();     // still on probation
int ota_health_confirm_s();    // seconds until confirmed (0 when not pending)
bool ota_health_rolled_back(); // the last update failed; the previous runs
