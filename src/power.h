#pragma once

// Deep sleep when nobody uses the camera (options().sleep_min), woken by
// moving it: the IMU's INT1 wired to options().imu_int. While asleep the
// board draws little, but is unreachable — no WiFi, no dashboard. A
// timelapse keeps going: a timer wakes it for each photo, which is taken
// without WiFi before it sleeps again.
//
// Sleep needs a way to wake by hand, so it's only allowed with the IMU and
// its wake wire; the BOOT/RST buttons and a power cycle always work too.

void power_init();               // first in setup(): why did we start?
const char *power_wake_cause();  // "power", "motion" or "timer"
bool power_woke_for_timelapse(); // setup() takes the photo and sleeps again
void power_activity();           // someone used the dashboard or the video
// Why it won't sleep, "" if it may (when idle long enough)
const char *power_sleep_blocker();
int power_idle_s();     // seconds since the last activity
void power_tick();      // loop(): sleep once idle
void power_sleep_now(); // prepare everything and enter deep sleep
