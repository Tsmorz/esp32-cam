#include "auth.h"
#include "config.h"
#include "settings.h"
#include "token_rules.h"
#include <Arduino.h>
#include <Preferences.h>
#include <atomic>
#include <cstring>

// Written once, while unset; readers look at _set first
static char _claimed[TOKEN_MAX + 1];
static std::atomic<bool> _set{false};

bool auth_from_secrets() { return API_TOKEN[0] != '\0'; }

void auth_init() {
  Preferences prefs;
  if (!prefs.begin(settings::SYS, auth_from_secrets() ? false : true))
    return;
  String t = prefs.getString(settings::TOKEN_KEY, "");
  if (auth_from_secrets()) {
    // Keep the compiled-in token in flash as well, so a later build without
    // secrets.h (CI, releases) still accepts it instead of starting unclaimed
    if (strcmp(t.c_str(), API_TOKEN)) {
      prefs.putString(settings::TOKEN_KEY, API_TOKEN);
      Serial.println("[auth] token from secrets.h saved to flash");
    }
    prefs.end();
    return;
  }
  prefs.end();
  if (token_valid(t.c_str())) {
    strcpy(_claimed, t.c_str());
    _set = true;
  }
}

const char *auth_token() {
  if (auth_from_secrets())
    return API_TOKEN;
  return _set ? _claimed : "";
}

bool auth_configured() { return auth_token()[0] != '\0'; }

ClaimResult auth_claim(const char *token) {
  if (auth_configured())
    return ClaimResult::AlreadySet;
  if (!token_valid(token))
    return ClaimResult::Invalid;
  Preferences prefs;
  prefs.begin(settings::SYS, false);
  prefs.putString(settings::TOKEN_KEY, token);
  prefs.end();
  strcpy(_claimed, token);
  _set = true;
  Serial.println("[auth] API token claimed");
  return ClaimResult::Ok;
}

void auth_forget() {
  Preferences prefs;
  if (prefs.begin(settings::SYS, false)) {
    prefs.remove(settings::TOKEN_KEY);
    prefs.end();
  }
}
