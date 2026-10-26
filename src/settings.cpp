#include "settings.h"
#include "auth.h"
#include "secret_store.h"
#include <Arduino.h>
#include <Preferences.h>
#include <cstring>
#include <nvs.h>
#include <nvs_flash.h>
#include <vector>

static int _stored_schema = 0;

static void clear_ns(const char *ns) {
  Preferences prefs;
  if (prefs.begin(ns, false)) {
    prefs.clear();
    prefs.end();
  }
}

// NVS only marks a deleted or overwritten value as erased; its bytes stay in
// flash until the page is reused. To get rid of plain-text passwords for
// good, copy everything out, erase the partition and write it back. Also
// drops the WiFi driver's own store (nvs.net80211), which an older build may
// have filled with the password: WiFi.persistent(false) keeps it empty now.
// A power cut in the middle loses the settings — a one-time risk, on the
// first boot of schema 3.
struct Entry {
  char ns[NVS_KEY_NAME_MAX_SIZE], key[NVS_KEY_NAME_MAX_SIZE];
  nvs_type_t type;
  uint64_t num;
  std::vector<uint8_t> data; // STR (with its NUL) and BLOB
};

static bool read_entry(const nvs_entry_info_t &info, Entry &e) {
  nvs_handle_t h;
  if (nvs_open(info.namespace_name, NVS_READONLY, &h) != ESP_OK)
    return false;
  strlcpy(e.ns, info.namespace_name, sizeof(e.ns));
  strlcpy(e.key, info.key, sizeof(e.key));
  e.type = info.type;
  e.num = 0;
  esp_err_t err = ESP_FAIL;
  size_t len = 0;
  switch (info.type) {
  case NVS_TYPE_U8:
    err = nvs_get_u8(h, info.key, (uint8_t *)&e.num);
    break;
  case NVS_TYPE_I8:
    err = nvs_get_i8(h, info.key, (int8_t *)&e.num);
    break;
  case NVS_TYPE_U16:
    err = nvs_get_u16(h, info.key, (uint16_t *)&e.num);
    break;
  case NVS_TYPE_I16:
    err = nvs_get_i16(h, info.key, (int16_t *)&e.num);
    break;
  case NVS_TYPE_U32:
    err = nvs_get_u32(h, info.key, (uint32_t *)&e.num);
    break;
  case NVS_TYPE_I32:
    err = nvs_get_i32(h, info.key, (int32_t *)&e.num);
    break;
  case NVS_TYPE_U64:
    err = nvs_get_u64(h, info.key, &e.num);
    break;
  case NVS_TYPE_I64:
    err = nvs_get_i64(h, info.key, (int64_t *)&e.num);
    break;
  case NVS_TYPE_STR:
    if ((err = nvs_get_str(h, info.key, nullptr, &len)) == ESP_OK) {
      e.data.resize(len);
      err = nvs_get_str(h, info.key, (char *)e.data.data(), &len);
    }
    break;
  case NVS_TYPE_BLOB:
    if ((err = nvs_get_blob(h, info.key, nullptr, &len)) == ESP_OK) {
      e.data.resize(len);
      err = nvs_get_blob(h, info.key, e.data.data(), &len);
    }
    break;
  default:
    break;
  }
  nvs_close(h);
  return err == ESP_OK;
}

static void write_entry(const Entry &e) {
  nvs_handle_t h;
  if (nvs_open(e.ns, NVS_READWRITE, &h) != ESP_OK)
    return;
  switch (e.type) {
  case NVS_TYPE_U8:
    nvs_set_u8(h, e.key, (uint8_t)e.num);
    break;
  case NVS_TYPE_I8:
    nvs_set_i8(h, e.key, (int8_t)e.num);
    break;
  case NVS_TYPE_U16:
    nvs_set_u16(h, e.key, (uint16_t)e.num);
    break;
  case NVS_TYPE_I16:
    nvs_set_i16(h, e.key, (int16_t)e.num);
    break;
  case NVS_TYPE_U32:
    nvs_set_u32(h, e.key, (uint32_t)e.num);
    break;
  case NVS_TYPE_I32:
    nvs_set_i32(h, e.key, (int32_t)e.num);
    break;
  case NVS_TYPE_U64:
    nvs_set_u64(h, e.key, e.num);
    break;
  case NVS_TYPE_I64:
    nvs_set_i64(h, e.key, (int64_t)e.num);
    break;
  case NVS_TYPE_STR:
    nvs_set_str(h, e.key, (const char *)e.data.data());
    break;
  case NVS_TYPE_BLOB:
    nvs_set_blob(h, e.key, e.data.data(), e.data.size());
    break;
  default:
    break;
  }
  nvs_commit(h);
  nvs_close(h);
}

static void settings_scrub() {
  std::vector<Entry> keep;
  nvs_iterator_t it = nullptr;
  esp_err_t r =
      nvs_entry_find(NVS_DEFAULT_PART_NAME, nullptr, NVS_TYPE_ANY, &it);
  while (r == ESP_OK) {
    nvs_entry_info_t info;
    nvs_entry_info(it, &info);
    Entry e;
    if (strcmp(info.namespace_name, "nvs.net80211") && read_entry(info, e))
      keep.push_back(std::move(e));
    r = nvs_entry_next(&it);
  }
  nvs_release_iterator(it);

  nvs_flash_deinit();
  if (nvs_flash_erase() != ESP_OK || nvs_flash_init() != ESP_OK) {
    Serial.println("[settings] scrub failed — settings may be lost");
    nvs_flash_init();
    return;
  }
  for (const Entry &e : keep)
    write_entry(e);
  Serial.printf("[settings] flash scrubbed, %u values kept\n",
                (unsigned)keep.size());
}

// Brings flash written by an older firmware up to SETTINGS_SCHEMA, oldest step
// first. Schema 0 → 1 changes nothing: an unversioned flash already used the
// v1 layout. Returns true if the flash needs scrubbing.
static bool migrate(int from) {
  if (from < 2) { // the tag detector was removed: drop its data
    clear_ns("det");
    clear_ns("calib");
  }
  return from < 3; // passwords encrypted: old plain ones may linger
}

void settings_init() {
  secret_store_init();
  Preferences prefs;
  if (!prefs.begin(settings::SYS, false))
    return;
  _stored_schema = prefs.getInt("schema", 0);
  prefs.end();

  bool scrub = false;
  if (_stored_schema > SETTINGS_SCHEMA) {
    // Flashed back to older firmware: leave the newer data alone, it may
    // still be readable
    Serial.printf("[settings] schema %d is newer than this firmware (%d)\n",
                  _stored_schema, SETTINGS_SCHEMA);
  } else if (_stored_schema < SETTINGS_SCHEMA) {
    scrub = migrate(_stored_schema);
  }
  // Plain-text passwords, at any schema: an older firmware flashed in between
  // writes them again
  scrub |= secret_adopt(settings::SYS, "token", settings::TOKEN_KEY);
  scrub |= secret_adopt(settings::WIFI, "pass", settings::WIFI_PASS_KEY);
  if (scrub)
    settings_scrub();

  if (_stored_schema < SETTINGS_SCHEMA && prefs.begin(settings::SYS, false)) {
    prefs.putInt("schema", SETTINGS_SCHEMA);
    prefs.end();
    _stored_schema = SETTINGS_SCHEMA;
  }
}

int settings_schema() { return _stored_schema; }

bool settings_reset(const char *scope) {
  const bool all = !strcmp(scope, "all");
  bool known = all;
  if (all || !strcmp(scope, "camera")) {
    clear_ns(settings::CAMERA);
    known = true;
  }
  if (all || !strcmp(scope, "token")) {
    auth_forget();
    known = true;
  }
  if (all || !strcmp(scope, "wifi")) {
    clear_ns(settings::WIFI);
    known = true;
  }
  if (all || !strcmp(scope, "imu")) {
    clear_ns(settings::IMU);
    known = true;
  }
  return known;
}
