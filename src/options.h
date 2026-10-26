#pragma once
#include <stddef.h>

// Everything the dashboard can switch on: what's wired to the board (memory
// card, the IMU's wake wire), automatic photos and clips, sleep, and animal
// detection. Kept in NVS (settings::OPTIONS); read by the tasks at any time.
//
// Wiring changes take effect after a restart (the SPI bus and wake pin are
// set up at boot); everything else at once.

enum Action { ACT_NONE = 0, ACT_PHOTO = 1, ACT_CLIP = 2 };

struct Options {
  // Wiring: a GPIO, or -1 = not connected
  int sd_sck = -1, sd_miso = -1, sd_mosi = -1, sd_cs = -1;
  int imu_int = -1; // LSM6DSOX INT1 → an RTC GPIO, wakes the board
  // Automatic photos and clips
  int timelapse_min = 0;     // a photo every N minutes, 0 = off
  int motion_action = 0;     // Action when the camera is moved
  int motion_sens = 2;       // 1 low, 2 medium, 3 high
  int clip_s = 10;           // length of an automatic clip
  int rec_max_min = 5;       // longest recording started by hand
  int sleep_min = 0;         // deep sleep after N idle minutes, 0 = never
  int detect_on = 0;         // run the uploaded model
  int detect_action = 0;     // Action when a watched-for animal is seen
  int detect_thr = 60;       // confidence, %
  char detect_only[96] = ""; // comma-separated labels, "" = any
};

void options_init(); // after settings_init()
const Options &options();
// false = unknown key or bad value (why: a short reason). restart: it only
// takes effect after a restart.
bool options_set(const char *key, const char *val, bool *restart,
                 const char **why);
size_t options_json(char *buf, size_t size);
bool options_pin_free(int gpio); // offered for wiring (not used on board)
bool options_wake_pin(int gpio); // can wake the chip (RTC GPIO)
// Is label among detect_only (or detect_only empty)?
bool options_watching(const char *label);
