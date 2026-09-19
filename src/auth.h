#pragma once

// The API token that protects every state-changing route and the UDP push.
//
// A build with API_TOKEN in secrets.h uses that, and also keeps it in flash,
// so the next build without secrets.h (every CI and release binary) still
// accepts it. With no token in flash either, the board starts unclaimed: the
// first client to POST /auth/claim chooses the token, and it is kept in flash
// from then on. Whoever reaches the board first wins — the same
// trust-on-first-use as a router's setup page — so claim it right after
// flashing. Forgetting it needs the token (POST /settings/reset?scope=token) or
// the serial command 'T'.

enum class ClaimResult { Ok, Invalid, AlreadySet };

void auth_init();         // after settings_init()
const char *auth_token(); // "" = none set yet
bool auth_configured();
bool auth_from_secrets(); // the token is compiled in and cannot be claimed
ClaimResult auth_claim(const char *token);
void auth_forget(); // clears a claimed token (not a compiled-in one)
