#pragma once
#include <stddef.h>

// Passwords at rest (the API token, the home WiFi password): AES-256-GCM
// blobs in NVS, so a copy of the flash doesn't give them away.
//
// The key comes from the chip's HMAC peripheral: a random 256-bit key burned
// once into an eFuse key block (purpose HMAC_UP, read-protected — software
// can't read it back, only use it), HMAC'd with a fixed label. IDF's own NVS
// encryption isn't usable here: Arduino mounts NVS unencrypted before setup(),
// and a new partition table can't be installed over the air.
//
// If no eFuse key block is free (or burning fails) the key is derived from the
// MAC address instead: that only obscures, and secret_store_protection() says
// so ("firmware" instead of "chip").

static constexpr size_t SECRET_MAX = 64; // longest value, without the NUL

// First thing in settings_init(): finds the chip key, or burns one
void secret_store_init();
const char *secret_store_protection(); // "chip" or "firmware"

bool secret_put(const char *ns, const char *key, const char *value);
// false if absent or not decryptable (out = "")
bool secret_get(const char *ns, const char *key, char *out, size_t outlen);
void secret_remove(const char *ns, const char *key);

// A value an older firmware stored in plain text under plain_key: encrypt it
// into key and delete the plain one. True if there was one (its bytes stay in
// flash until settings_scrub()).
bool secret_adopt(const char *ns, const char *plain_key, const char *key);
