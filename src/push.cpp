#include "push.h"
#include "config.h"
#include "detections_json.h"
#include "detector.h"
#include "push_message.h"
#include <Arduino.h>
#include <esp_timer.h>
#include <lwip/sockets.h>

static constexpr int MAX_SUBS = 4;
static constexpr uint32_t SUB_TTL_MS = 10000;

struct Sub {
  sockaddr_in addr;
  uint32_t seen_ms;
  bool used;
};
static Sub _subs[MAX_SUBS];
static int _sock = -1;
static portMUX_TYPE _mux = portMUX_INITIALIZER_UNLOCKED;

static bool same(const sockaddr_in &a, const sockaddr_in &b) {
  return a.sin_addr.s_addr == b.sin_addr.s_addr && a.sin_port == b.sin_port;
}

static void subscribe(const sockaddr_in &from, bool on) {
  portENTER_CRITICAL(&_mux);
  int free_slot = -1;
  bool found = false;
  for (int i = 0; i < MAX_SUBS; i++) {
    if (_subs[i].used && same(_subs[i].addr, from)) {
      found = true;
      if (on)
        _subs[i].seen_ms = millis();
      else
        _subs[i].used = false;
    } else if (!_subs[i].used && free_slot < 0) {
      free_slot = i;
    }
  }
  if (on && !found && free_slot >= 0)
    _subs[free_slot] = {from, millis(), true};
  portEXIT_CRITICAL(&_mux);
}

static void push_task(void *) {
  char msg[80]; // "sub " + a token of up to 64 characters
  for (;;) {
    sockaddr_in from;
    socklen_t len = sizeof(from);
    const int n = recvfrom(_sock, msg, sizeof(msg) - 1, 0, (sockaddr *)&from,
                           &len); // times out every 500 ms
    if (n >= 0) {
      const PushCmd cmd = push_parse(msg, n, API_TOKEN);
      if (cmd != PushCmd::None)
        subscribe(from, cmd == PushCmd::Sub);
    }
    const uint32_t now = millis();
    portENTER_CRITICAL(&_mux);
    for (Sub &s : _subs)
      if (s.used && now - s.seen_ms > SUB_TTL_MS)
        s.used = false;
    portEXIT_CRITICAL(&_mux);
  }
}

void push_start(BaseType_t core, UBaseType_t prio) {
  _sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (_sock < 0) {
    Serial.println("[push] no socket");
    return;
  }
  sockaddr_in a = {};
  a.sin_family = AF_INET;
  a.sin_port = htons(PUSH_PORT);
  a.sin_addr.s_addr = htonl(INADDR_ANY);
  timeval tv = {0, 500000};
  setsockopt(_sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  if (bind(_sock, (sockaddr *)&a, sizeof(a)) < 0) {
    Serial.println("[push] bind failed");
    close(_sock);
    _sock = -1;
    return;
  }
  xTaskCreatePinnedToCore(push_task, "push", 3072, nullptr, prio, nullptr,
                          core);
  Serial.printf("[push] UDP :%d — send \"sub <API_TOKEN>\" to subscribe%s\n",
                PUSH_PORT,
                API_TOKEN[0] ? "" : " (disabled: API_TOKEN not set)");
}

int push_subscribers() {
  int n = 0;
  portENTER_CRITICAL(&_mux);
  for (const Sub &s : _subs)
    n += s.used;
  portEXIT_CRITICAL(&_mux);
  return n;
}

void push_frame(const DetectionFrame &f) {
  if (_sock < 0)
    return;
  sockaddr_in to[MAX_SUBS];
  int n = 0;
  portENTER_CRITICAL(&_mux);
  for (const Sub &s : _subs)
    if (s.used)
      to[n++] = s.addr;
  portEXIT_CRITICAL(&_mux);
  if (!n)
    return;

  static char buf[1472]; // one datagram; only the detector task gets here
  const DetectionsJson a = {false, false, TAG_SIZE_M, millis(),
                            esp_timer_get_time()};
  const size_t len = detections_json(f, a, buf, sizeof(buf));
  for (int i = 0; i < n; i++)
    sendto(_sock, buf, len, 0, (sockaddr *)&to[i], sizeof(to[i]));
}
