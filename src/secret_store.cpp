#include "secret_store.h"
#include "settings.h"
#include <Arduino.h>
#include <Preferences.h>
#include <bootloader_random.h>
#include <cstring>
#include <esp_efuse.h>
#include <esp_hmac.h>
#include <esp_mac.h>
#include <esp_random.h>
#include <mbedtls/gcm.h>
#include <mbedtls/sha256.h>

// Blob: version | nonce (12) | tag (16) | ciphertext. The version says which
// key sealed it. The nonce is a counter kept in NVS plus 4 random bytes, so it
// never repeats even if the RNG has little entropy this early in boot.
enum : uint8_t { V_CHIP = 1, V_FIRMWARE = 2 };
static constexpr size_t NONCE = 12, TAG = 16, HDR = 1 + NONCE + TAG;
static constexpr const char *NONCE_KEY = "nonce"; // in SYS

static uint8_t _chip[32], _fw[32];
static bool _have_chip = false;

static bool chip_key() {
  esp_efuse_block_t blk;
  if (!esp_efuse_find_purpose(ESP_EFUSE_KEY_PURPOSE_HMAC_UP, &blk)) {
    blk = esp_efuse_find_unused_key_block();
    if (blk == EFUSE_BLK_KEY_MAX) {
      Serial.println("[secrets] no free eFuse key block");
      return false;
    }
    // Permanent: the block keeps this key and purpose for the chip's life
    uint8_t k[32];
    bootloader_random_enable(); // true entropy before the radio is on
    esp_fill_random(k, sizeof(k));
    bootloader_random_disable();
    const esp_err_t err =
        esp_efuse_write_key(blk, ESP_EFUSE_KEY_PURPOSE_HMAC_UP, k, sizeof(k));
    memset(k, 0, sizeof(k));
    if (err != ESP_OK) {
      Serial.printf("[secrets] burning the chip key failed: 0x%x\n", err);
      return false;
    }
    Serial.printf("[secrets] chip key burned into eFuse block %d\n", (int)blk);
  }
  static const char LABEL[] = "wifi-camera secrets v1";
  const hmac_key_id_t id = (hmac_key_id_t)(blk - EFUSE_BLK_KEY0);
  return esp_hmac_calculate(id, LABEL, sizeof(LABEL) - 1, _chip) == ESP_OK;
}

void secret_store_init() {
  uint8_t mac[6] = {};
  esp_efuse_mac_get_default(mac);
  static const char LABEL[] = "wifi-camera secrets fallback v1";
  mbedtls_sha256_context h;
  mbedtls_sha256_init(&h);
  mbedtls_sha256_starts(&h, 0);
  mbedtls_sha256_update(&h, (const uint8_t *)LABEL, sizeof(LABEL) - 1);
  mbedtls_sha256_update(&h, mac, sizeof(mac));
  mbedtls_sha256_finish(&h, _fw);
  mbedtls_sha256_free(&h);

  _have_chip = chip_key();
  if (!_have_chip)
    Serial.println("[secrets] WARNING: passwords only obscured (no chip key)");
}

const char *secret_store_protection() {
  return _have_chip ? "chip" : "firmware";
}

static void aad(const char *ns, const char *key, char *out, size_t n) {
  snprintf(out, n, "%s/%s", ns, key); // binds a blob to its slot
}

bool secret_put(const char *ns, const char *key, const char *value) {
  const size_t n = strlen(value);
  if (n > SECRET_MAX)
    return false;
  uint8_t blob[HDR + SECRET_MAX];
  blob[0] = _have_chip ? V_CHIP : V_FIRMWARE;

  Preferences prefs;
  if (!prefs.begin(settings::SYS, false))
    return false;
  const uint64_t ctr = prefs.getULong64(NONCE_KEY, 0) + 1;
  prefs.putULong64(NONCE_KEY, ctr);
  prefs.end();
  memcpy(blob + 1, &ctr, sizeof(ctr));
  esp_fill_random(blob + 1 + sizeof(ctr), NONCE - sizeof(ctr));

  char ad[40];
  aad(ns, key, ad, sizeof(ad));
  mbedtls_gcm_context g;
  mbedtls_gcm_init(&g);
  const bool ok =
      mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES,
                         blob[0] == V_CHIP ? _chip : _fw, 256) == 0 &&
      mbedtls_gcm_crypt_and_tag(&g, MBEDTLS_GCM_ENCRYPT, n, blob + 1, NONCE,
                                (const uint8_t *)ad, strlen(ad),
                                (const uint8_t *)value, blob + HDR, TAG,
                                blob + 1 + NONCE) == 0;
  mbedtls_gcm_free(&g);
  if (!ok || !prefs.begin(ns, false))
    return false;
  const bool stored = prefs.putBytes(key, blob, HDR + n) == HDR + n;
  prefs.end();
  return stored;
}

bool secret_get(const char *ns, const char *key, char *out, size_t outlen) {
  out[0] = '\0';
  uint8_t blob[HDR + SECRET_MAX];
  Preferences prefs;
  if (!prefs.begin(ns, true))
    return false;
  const size_t len = prefs.getBytesLength(key);
  const bool read = len >= HDR && len <= sizeof(blob) &&
                    prefs.getBytes(key, blob, len) == len;
  prefs.end();
  const size_t n = len - HDR;
  if (!read || n + 1 > outlen || (blob[0] == V_CHIP && !_have_chip) ||
      (blob[0] != V_CHIP && blob[0] != V_FIRMWARE))
    return false;

  char ad[40];
  aad(ns, key, ad, sizeof(ad));
  mbedtls_gcm_context g;
  mbedtls_gcm_init(&g);
  const bool ok =
      mbedtls_gcm_setkey(&g, MBEDTLS_CIPHER_ID_AES,
                         blob[0] == V_CHIP ? _chip : _fw, 256) == 0 &&
      mbedtls_gcm_auth_decrypt(&g, n, blob + 1, NONCE, (const uint8_t *)ad,
                               strlen(ad), blob + 1 + NONCE, TAG, blob + HDR,
                               (uint8_t *)out) == 0;
  mbedtls_gcm_free(&g);
  out[ok ? n : 0] = '\0';
  if (!ok)
    Serial.printf("[secrets] %s/%s can't be decrypted\n", ns, key);
  return ok;
}

void secret_remove(const char *ns, const char *key) {
  Preferences prefs;
  if (prefs.begin(ns, false)) {
    prefs.remove(key);
    prefs.end();
  }
}

bool secret_adopt(const char *ns, const char *plain_key, const char *key) {
  Preferences prefs;
  if (!prefs.begin(ns, false))
    return false;
  if (prefs.getType(plain_key) != PT_STR) {
    prefs.end();
    return false;
  }
  const String v = prefs.getString(plain_key, "");
  prefs.end();
  if (v.length() && !secret_put(ns, key, v.c_str()))
    return false; // keep the plain one rather than lose it
  secret_remove(ns, plain_key);
  Serial.printf("[secrets] %s/%s encrypted\n", ns, key);
  return true;
}
