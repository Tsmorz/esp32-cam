#include "auth.h"
#include "config.h"
#include "secret_store.h"
#include "settings.h"
#include "token_rules.h"
#include <Arduino.h>
#include <atomic>
#include <cstring>
#include <mbedtls/sha256.h>

static_assert(!API_TOKEN[0] || token_valid(API_TOKEN),
              "API_TOKEN in secrets.h: 8-63 printable characters, no spaces");

// Written once, while unset; readers look at _set first
static char _claimed[TOKEN_MAX + 1];
static char _cookie[65]; // hex SHA-256 of the token, see auth_video_cookie()
static std::atomic<bool> _set{false};

bool auth_from_secrets() { return API_TOKEN[0] != '\0'; }

static void make_cookie(const char *token) {
  static const char LABEL[] = "wifi-camera video v1:";
  uint8_t d[32];
  mbedtls_sha256_context h;
  mbedtls_sha256_init(&h);
  mbedtls_sha256_starts(&h, 0);
  mbedtls_sha256_update(&h, (const uint8_t *)LABEL, sizeof(LABEL) - 1);
  mbedtls_sha256_update(&h, (const uint8_t *)token, strlen(token));
  mbedtls_sha256_finish(&h, d);
  mbedtls_sha256_free(&h);
  for (int i = 0; i < 32; i++)
    snprintf(_cookie + 2 * i, 3, "%02x", d[i]);
}

void auth_init() {
  char t[TOKEN_MAX + 1];
  secret_get(settings::SYS, settings::TOKEN_KEY, t, sizeof(t));
  if (auth_from_secrets()) {
    // Keep the compiled-in token in flash as well, so a later build without
    // secrets.h (CI, releases) still accepts it instead of starting unclaimed
    if (strcmp(t, API_TOKEN) &&
        secret_put(settings::SYS, settings::TOKEN_KEY, API_TOKEN))
      Serial.println("[auth] token from secrets.h saved to flash");
    make_cookie(API_TOKEN);
    return;
  }
  if (token_valid(t)) {
    strcpy(_claimed, t);
    make_cookie(t);
    _set = true;
  }
}

const char *auth_token() {
  if (auth_from_secrets())
    return API_TOKEN;
  return _set ? _claimed : "";
}

bool auth_configured() { return auth_token()[0] != '\0'; }

const char *auth_video_cookie() { return auth_configured() ? _cookie : ""; }

ClaimResult auth_claim(const char *token) {
  if (auth_configured())
    return ClaimResult::AlreadySet;
  if (!token_valid(token))
    return ClaimResult::Invalid;
  if (!secret_put(settings::SYS, settings::TOKEN_KEY, token))
    return ClaimResult::Invalid;
  strcpy(_claimed, token);
  make_cookie(token);
  _set = true;
  Serial.println("[auth] API token claimed");
  return ClaimResult::Ok;
}

void auth_forget() {
  secret_remove(settings::SYS, settings::TOKEN_KEY);
  _set = false;
}
