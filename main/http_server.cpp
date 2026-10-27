#include "http_server.h"
#include "auth.h"
#include "build_info.h"
#include "camera.h"
#include "camera_profile.h"
#include "config.h"
#include "detect.h"
#include "focus.h"
#include "generated/web_index.h"
#include "hostname_rules.h"
#include "http_util.h"
#include "imu.h"
#include "led.h"
#include "options.h"
#include "ota_health.h"
#include "power.h"
#include "recorder.h"
#include "secret_store.h"
#include "settings.h"
#include "storage.h"
#include "strbuf.h"
#include "system.h"
#include "token_rules.h"
#include "wifi_mgr.h"
#include <Arduino.h>
#include <WiFi.h>
#include <esp_camera.h>
#include <esp_http_server.h>
#include <esp_ota_ops.h>
#include <esp_partition.h>

static httpd_handle_t server = nullptr;
static httpd_handle_t stream_server = nullptr;

// ── /stream (control server) → stream server
// ──────────────────────────────────────────────────────────────────
static esp_err_t stream_redirect_handler(httpd_req_t *req) {
  char host[64] = "192.168.4.1";
  httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host));
  char *colon = strchr(host, ':');
  if (colon)
    *colon = '\0';
  char loc[96];
  snprintf(loc, sizeof(loc), "http://%s:%d/stream", host, STREAM_PORT);
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", loc);
  httpd_resp_send(req, nullptr, 0);
  return ESP_OK;
}

// ── /snapshot
// ─────────────────────────────────────────────────────────────────
static esp_err_t snapshot_handler(httpd_req_t *req) {
  if (!video_allowed(req))
    return ESP_FAIL;
  camera_fb_t *fb = camera_fb_get_at_stream_size();
  if (!fb) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No frame");
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "image/jpeg");
  esp_err_t res = httpd_resp_send(req, (const char *)fb->buf, fb->len);
  camera_fb_return_at_stream_size(fb);
  return res;
}

// ── Anything else: captive portal, or a plain 404
// ─────────────────────────────────────────────────────────────────
// A phone that joins the hotspot asks some well-known host (captive.apple.com,
// connectivitycheck.gstatic.com, www.msftconnecttest.com…) for a test page;
// the DNS server sends it here. Answering with a redirect to the dashboard
// makes the phone open it as a sign-in page. Requests addressed to the camera
// itself still get a 404. Known routes do the same through guarded().
static esp_err_t not_found_handler(httpd_req_t *req, httpd_err_code_t) {
  if (!host_ok(req))
    return redirect_home(req);
  httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
  return ESP_FAIL;
}

// ── /pose — orientation from the IMU
// ─────────────────────────────────────────────────────────────────
static esp_err_t pose_handler(httpd_req_t *req) {
  if (!video_allowed(req))
    return ESP_FAIL;
  double q[4];
  imu_get_quat(q);
  uint32_t n, err;
  const char *state = imu_state(&n, &err);
  float f[3];
  const bool field = imu_mag_field(f);
  char buf[320];
  int len = snprintf(
      buf, sizeof(buf),
      "{\"q\":[%.6f,%.6f,%.6f,%.6f],\"imu\":\"%s\",\"n\":%lu,\"err\":%lu,"
      "\"mag\":\"%s\",\"heading\":\"%s\",\"cal\":%.2f,"
      "\"field_ut\":%.1f}",
      q[0], q[1], q[2], q[3], state, (unsigned long)n, (unsigned long)err,
      imu_mag_state(), imu_heading_from_mag() ? "mag" : "gyro",
      imu_mag_cal_progress(),
      field ? sqrtf(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]) : 0.0f);
  return send_json(req, buf, len);
}

// ── /imu/compass?do=start|finish|cancel|clear — compass calibration
// ─────────────────────────────────────────────────────────────────
static esp_err_t compass_handler(httpd_req_t *req) {
  if (!authorized(req))
    return ESP_FAIL;
  char qs[32] = {}, act[12] = {};
  if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) != ESP_OK ||
      httpd_query_key_value(qs, "do", act, sizeof(act)) != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                        "do = start|finish|cancel|clear");
    return ESP_FAIL;
  }
  bool ok = true;
  const char *why = "do = start|finish|cancel|clear";
  if (!strcmp(act, "start")) {
    ok = imu_mag_cal_start();
    why = "No magnetometer";
  } else if (!strcmp(act, "finish")) {
    ok = imu_mag_cal_finish();
    why = "Not enough rotation yet: keep turning the board in every "
          "direction, upside down too";
  } else if (!strcmp(act, "cancel")) {
    imu_mag_cal_cancel();
  } else if (!strcmp(act, "clear")) {
    imu_mag_cal_clear();
  } else {
    ok = false;
  }
  if (!ok) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, why);
    return ESP_FAIL;
  }
  httpd_resp_sendstr(req, "OK");
  return ESP_OK;
}

// ── / (dashboard: web/index.html, gzipped into the firmware at build time)
// ─────────────────────────────────────────────────────────────────────
static esp_err_t index_handler(httpd_req_t *req) {
  httpd_resp_set_type(req, "text/html");
  httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
  httpd_resp_set_hdr(req, "Cache-Control", "no-cache");
  return httpd_resp_send(req, (const char *)WEB_INDEX_GZ, WEB_INDEX_GZ_LEN);
}

// ── /status — everything the dashboard's header and System page show
// ─────────────────────────────────────────────────────────────────────
static esp_err_t status_handler(httpd_req_t *req) {
  char buf[1600];
  size_t pos = 0;
  const sensor_t *sen = esp_camera_sensor_get();
  const bool sta = WiFi.status() == WL_CONNECTED;
  char ap_ssid[68], sta_ssid[68], saved[68], joining[68], raw[33];
  json_esc(AP_SSID, ap_ssid, sizeof(ap_ssid));
  json_esc(sta ? WiFi.SSID().c_str() : "", sta_ssid, sizeof(sta_ssid));
  wifi_saved_ssid(raw, sizeof(raw));
  json_esc(raw, saved, sizeof(saved));
  wifi_joining(raw, sizeof(raw));
  json_esc(raw, joining, sizeof(joining));
  uint32_t imu_n, imu_err;
  const char *imu = imu_state(&imu_n, &imu_err);
  char hostname[HOSTNAME_MAX + 1];
  wifi_hostname(hostname, sizeof(hostname));

  appendf(buf, sizeof(buf), pos,
          "{\"uptime\":%lu,\"version\":\"%s\",\"build\":\"%s\",\"heap\":%u,"
          "\"psram\":%u,"
          "\"psram_total\":%u,\"temp_c\":%.1f,\"stream_port\":%d,\"auth\":%s,"
          "\"mirror\":%s,"
          "\"sensor\":\"%s\",\"af\":\"%s\",\"hostname\":\"%s\","
          "\"schema\":%d,\"secrets\":\"%s\",\"ota_pending\":%s,"
          "\"ota_confirm_s\":%d,\"rolled_back\":%s,\"hotspot\":%s,",
          (unsigned long)(millis() / 1000), fw_version(), fw_built(),
          (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram(),
          (unsigned)ESP.getPsramSize(), temperatureRead(), STREAM_PORT,
          auth_configured() ? "true" : "false",
          sen && sen->status.hmirror ? "true" : "false", camera_sensor_name(),
          focus_state(), hostname, settings_schema(), secret_store_protection(),
          ota_health_pending() ? "true" : "false", ota_health_confirm_s(),
          ota_health_rolled_back() ? "true" : "false",
          via_hotspot(req) ? "true" : "false");
  appendf(buf, sizeof(buf), pos,
          "\"imu\":{\"state\":\"%s\",\"samples\":%lu,\"errors\":%lu,"
          "\"mag\":\"%s\"},",
          imu, (unsigned long)imu_n, (unsigned long)imu_err, imu_mag_state());
  // Memory card, sleep, detection: what the Photos and Settings pages show
  appendf(buf, sizeof(buf), pos,
          "\"storage\":{\"state\":\"%s\",\"total\":%llu,\"used\":%llu,"
          "\"recording\":%s,\"rec_s\":%d,\"why\":\"%s\"},"
          "\"power\":{\"wake\":\"%s\",\"blocker\":\"%s\",\"idle_s\":%d},"
          "\"detect\":{\"state\":\"%s\"},",
          storage_state(), storage_total(), storage_used(),
          recorder_recording() ? "true" : "false", recorder_rec_s(),
          recorder_why(), power_wake_cause(), power_sleep_blocker(),
          power_idle_s(), detect_state());
  // ap_on: wifi_tick() turns the hotspot off while STA is up and nobody uses
  // it. ap_key: its WiFi password is the default, the camera's or AP_PASS.
  appendf(buf, sizeof(buf), pos,
          "\"wifi\":{\"ap_on\":%s,\"ap_ssid\":\"%s\",\"ap_ip\":\"%s\","
          "\"ap_key\":\"%s\",\"ap_clients\":%d,\"sta_ssid\":\"%s\","
          "\"sta_ip\":\"%s\",\"rssi\":%d,\"saved\":\"%s\",\"joining\":\"%s\"}}",
          (WiFi.getMode() & WIFI_AP) ? "true" : "false", ap_ssid,
          WiFi.softAPIP().toString().c_str(), wifi_ap_key(),
          WiFi.softAPgetStationNum(), sta_ssid,
          sta ? WiFi.localIP().toString().c_str() : "", sta ? WiFi.RSSI() : 0,
          saved, joining);

  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, buf, pos);
  return ESP_OK;
}

// ── /camera/status — current sensor settings, to populate the Camera page
// ─────────────────────────────────────────────────────────────────────
static esp_err_t camera_status_handler(httpd_req_t *req) {
  char buf[896];
  size_t len = camera_status_json(buf, sizeof(buf));
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, buf, len);
  return ESP_OK;
}

// The video cookie (auth.h): sent with the reply to a right password. Cookies
// ignore the port, so it reaches the stream server on :81 too.
static void set_video_cookie(httpd_req_t *req, char *buf, size_t size) {
  snprintf(buf, size,
           "%s=%s; Path=/; Max-Age=31536000; HttpOnly; SameSite=Strict",
           AUTH_COOKIE, auth_video_cookie());
  httpd_resp_set_hdr(req, "Set-Cookie", buf);
}

// ── /auth — is the token right? Lets the dashboard find out before it sends a
// firmware image: a rejected upload comes back to the browser as a dropped
// connection, not as the 401. Also hands out the video cookie.
static esp_err_t auth_handler(httpd_req_t *req) {
  if (!authorized(req))
    return ESP_FAIL;
  char cookie[160];
  set_video_cookie(req, cookie, sizeof(cookie));
  httpd_resp_sendstr(req, "OK");
  return ESP_OK;
}

// ── /auth/forget — drop this browser's video cookie (the dashboard's
// "Forget"). Needs nothing: all it can do is log someone out.
static esp_err_t auth_forget_handler(httpd_req_t *req) {
  char cookie[96];
  snprintf(cookie, sizeof(cookie),
           "%s=; Path=/; Max-Age=0; HttpOnly; SameSite=Strict", AUTH_COOKIE);
  httpd_resp_set_hdr(req, "Set-Cookie", cookie);
  httpd_resp_sendstr(req, "OK");
  return ESP_OK;
}

// ── /auth/claim — set the token while none is set. The custom header makes a
// cross-site page need a preflight, which is never answered.
static esp_err_t auth_claim_handler(httpd_req_t *req) {
  char tok[TOKEN_MAX + 2];
  char hdr[4];
  if (httpd_req_get_hdr_value_str(req, "X-Claim", hdr, sizeof(hdr)) != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing X-Claim header");
    return ESP_FAIL;
  }
  if (!read_body(req, tok, sizeof(tok)))
    return ESP_FAIL;
  char cookie[160];
  switch (auth_claim(tok)) {
  case ClaimResult::Ok:
    set_video_cookie(req, cookie, sizeof(cookie));
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
  case ClaimResult::AlreadySet:
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                        "This camera already has a password");
    return ESP_FAIL;
  default:
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                        "Use 8 to 63 characters: letters, numbers and "
                        "symbols, without spaces or accents");
    return ESP_FAIL;
  }
}

// ── /system/reboot, /camera/reset
// ─────────────────────────────────────────────────────────────────────
static esp_err_t reboot_handler(httpd_req_t *req) {
  if (!authorized(req))
    return ESP_FAIL;
  httpd_resp_sendstr(req, "OK");
  system_restart_in(300);
  return ESP_OK;
}

static esp_err_t camera_reset_handler(httpd_req_t *req) {
  if (!authorized(req))
    return ESP_FAIL;
  camera_reset_settings(); // defaults apply on the next boot
  httpd_resp_sendstr(req, "OK");
  system_restart_in(300);
  return ESP_OK;
}

// ── /settings/reset?scope=all|camera|wifi|imu|token
// ─────────────────────────────────────────────────────────────────────
static esp_err_t settings_reset_handler(httpd_req_t *req) {
  if (!authorized(req))
    return ESP_FAIL;
  char qs[32] = {}, scope[16] = {};
  if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) != ESP_OK ||
      httpd_query_key_value(qs, "scope", scope, sizeof(scope)) != ESP_OK ||
      !settings_reset(scope)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                        "scope = all|camera|wifi|imu|token");
    return ESP_FAIL;
  }
  httpd_resp_sendstr(req, "OK");
  system_restart_in(300); // settings are read at boot
  return ESP_OK;
}

// ── /wifi/scan
// ───────────────────────────────────────────────────────────────────
static esp_err_t wifi_scan_handler(httpd_req_t *req) {
  // Scanning takes a couple of seconds and this task serves every other route,
  // so it never waits: the first call starts a scan and answers
  // {"scanning":true}; ask again until the results are there. ?refresh=1
  // throws the last results away and scans again.
  char qs[24] = {};
  const bool refresh =
      httpd_req_get_url_query_str(req, qs, sizeof(qs)) == ESP_OK &&
      strstr(qs, "refresh=1");
  const int n = wifi_scan(refresh);
  if (n == WIFI_SCAN_RUNNING) {
    const char *msg = "{\"scanning\":true}";
    return send_json(req, msg, strlen(msg));
  }

  static constexpr size_t BUF = 4096;
  char *buf = (char *)malloc(BUF); // too big for the httpd task stack
  if (!buf) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    return ESP_FAIL;
  }
  char esc[68];
  size_t pos = 0;
  json_esc(WiFi.SSID().c_str(), esc, sizeof(esc));
  appendf(buf, BUF, pos, "{\"scanning\":false,\"sta\":\"%s\",\"networks\":[",
          esc);
  for (int i = 0; i < n && i < 24; i++) {
    json_esc(WiFi.SSID(i).c_str(), esc, sizeof(esc));
    appendf(buf, BUF, pos, "%s{\"ssid\":\"%s\",\"rssi\":%d,\"enc\":%d}",
            i > 0 ? "," : "", esc, (int)WiFi.RSSI(i),
            (int)(WiFi.encryptionType(i) != WIFI_AUTH_OPEN));
  }
  appendf(buf, BUF, pos, "]}");
  const esp_err_t res = send_json(req, buf, pos);
  free(buf);
  return res;
}

// ── /wifi/connect
// ────────────────────────────────────────────────────────────────
// ── /wifi/hostname?name= — the camera's name: http://<name>.local
// ─────────────────────────────────────────────────────────────────
static esp_err_t hostname_handler(httpd_req_t *req) {
  if (!authorized(req))
    return ESP_FAIL;
  char qs[64] = {}, name[HOSTNAME_MAX + 2] = {};
  if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) != ESP_OK ||
      httpd_query_key_value(qs, "name", name, sizeof(name)) != ESP_OK ||
      !wifi_set_hostname(name)) { // announced by wifi_tick()
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                        "Use 1 to 32 lowercase letters, numbers and dashes, "
                        "not starting or ending with a dash");
    return ESP_FAIL;
  }
  httpd_resp_sendstr(req, "OK");
  return ESP_OK;
}

static esp_err_t wifi_connect_handler(httpd_req_t *req) {
  if (!authorized(req))
    return ESP_FAIL;
  char body[256] = {};
  if (!read_body(req, body, sizeof(body)))
    return ESP_FAIL;
  char ssid[33] = {}, pass[65] = {};
  if (!json_str(body, "ssid", ssid, sizeof(ssid)) || !ssid[0]) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing ssid");
    return ESP_FAIL;
  }
  json_str(body, "pass", pass, sizeof(pass));
  memset(body, 0, sizeof(body));

  httpd_resp_sendstr(req, "OK"); // before the link may change under this reply
  wifi_request_join(ssid, pass); // loop() joins; saved only once it works
  memset(pass, 0, sizeof(pass));
  return ESP_OK;
}

// ── /ota
// ────────────────────────────────────────────────────────────────────────
// Uses ESP-IDF esp_ota_ops directly — avoids Arduino Update.h global
// constructor (UpdateClass Update) which crashes before setup() on OPI PSRAM.
static esp_err_t ota_handler(httpd_req_t *req) {
  if (!authorized(req))
    return ESP_FAIL; // closes the socket instead of draining the upload
  if (ota_health_pending()) {
    // IDF refuses to write the other slot while this image is on probation:
    // that slot is what a rollback would boot
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                        "Still finishing the last update — try again in a "
                        "few seconds");
    return ESP_FAIL;
  }
  const esp_partition_t *dst = esp_ota_get_next_update_partition(NULL);
  if (!dst || dst == esp_ota_get_running_partition()) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "No OTA partition — check partition table");
    return ESP_FAIL;
  }
  led_set(LED_OTA);
  size_t total = req->content_len;
  esp_ota_handle_t ota;
  if (esp_ota_begin(dst, total > 0 ? total : OTA_SIZE_UNKNOWN, &ota) !=
      ESP_OK) {
    led_restore();
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "OTA begin failed");
    return ESP_FAIL;
  }
  uint8_t buf[2048];
  size_t remaining = total;
  bool ok = true;
  int timeouts = 0;
  while (remaining > 0 || total == 0) {
    size_t to_read =
        (remaining > 0 && remaining < sizeof(buf)) ? remaining : sizeof(buf);
    int recv = httpd_req_recv(req, (char *)buf, to_read);
    if (recv == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 3)
      continue; // a slow link, not a dropped one
    if (recv == 0)
      break;
    if (recv < 0 || esp_ota_write(ota, buf, recv) != ESP_OK) {
      ok = false;
      break;
    }
    timeouts = 0;
    if (total > 0)
      remaining -= recv;
  }
  if (!ok)
    esp_ota_abort(ota);
  if (!ok || esp_ota_end(ota) != ESP_OK ||
      esp_ota_set_boot_partition(dst) != ESP_OK) {
    led_restore();
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "OTA write failed");
    return ESP_FAIL;
  }
  httpd_resp_sendstr(req, "OK");
  system_restart_in(500);
  return ESP_OK;
}

// ── /camera/set
// ─────────────────────────────────────────────────────────────────
static esp_err_t camera_set_handler(httpd_req_t *req) {
  if (!authorized(req))
    return ESP_FAIL;
  char qs[160] = {}, var[32] = {}, val[96] = {};
  const bool got =
      httpd_req_get_url_query_str(req, qs, sizeof(qs)) == ESP_OK &&
      httpd_query_key_value(qs, "var", var, sizeof(var)) == ESP_OK &&
      httpd_query_key_value(qs, "val", val, sizeof(val)) == ESP_OK;
  url_decode(val); // the corners' commas may come as %2C
  // quad: the straightening's corners, "" = none (camera_set_quad)
  if (!got || !(strcmp(var, "quad") ? camera_set(var, atoi(val))
                                    : camera_set_quad(val))) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad var or val");
    return ESP_FAIL;
  }
  httpd_resp_sendstr(req, "OK");
  return ESP_OK;
}

// ── Options: wiring, automation, sleep, detection
// ─────────────────────────────────────────────────────────────────
static esp_err_t options_handler(httpd_req_t *req) {
  char buf[768];
  size_t n = options_json(buf, sizeof(buf));
  return send_json(req, buf, n);
}

// POST /options/set?key=&val= — one at a time, as the dashboard changes it
static esp_err_t options_set_handler(httpd_req_t *req) {
  if (!authorized(req))
    return ESP_FAIL;
  char qs[200] = {}, key[24] = {}, val[128] = {};
  if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) != ESP_OK ||
      httpd_query_key_value(qs, "key", key, sizeof(key)) != ESP_OK ||
      httpd_query_key_value(qs, "val", val, sizeof(val)) == ESP_ERR_NOT_FOUND) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "key and val");
    return ESP_FAIL;
  }
  url_decode(val);
  bool restart;
  const char *why;
  if (!options_set(key, val, &restart, &why)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, why);
    return ESP_FAIL;
  }
  const char *msg = restart ? "{\"restart\":true}" : "{\"restart\":false}";
  return send_json(req, msg, strlen(msg));
}

// ── Memory card: gallery, files, photos and clips
// ─────────────────────────────────────────────────────────────────
static int query_int(const char *qs, const char *key, int def) {
  char v[12];
  return httpd_query_key_value(qs, key, v, sizeof(v)) == ESP_OK ? atoi(v) : def;
}

static esp_err_t files_handler(httpd_req_t *req) {
  if (!video_allowed(req))
    return ESP_FAIL;
  char qs[48] = {};
  httpd_req_get_url_query_str(req, qs, sizeof(qs));
  const int offset = query_int(qs, "offset", 0);
  const int limit = query_int(qs, "limit", 48);
  static constexpr size_t BUF = 8192; // 48 × ~60 bytes
  char *buf = (char *)malloc(BUF);
  if (!buf) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    return ESP_FAIL;
  }
  const size_t len = storage_list_json(buf, BUF, offset < 0 ? 0 : offset,
                                       limit < 1 || limit > 100 ? 48 : limit);
  const esp_err_t r = send_json(req, buf, len);
  free(buf);
  return r;
}

// Files are sent by a worker task (httpd's async handlers): a clip of
// several MB would otherwise hold the control task — and every other route —
// for the whole download
static QueueHandle_t _file_q = nullptr;

static bool file_name(httpd_req_t *req, char *name, size_t n, char *path,
                      size_t pn) {
  char qs[48] = {};
  return httpd_req_get_url_query_str(req, qs, sizeof(qs)) == ESP_OK &&
         httpd_query_key_value(qs, "name", name, n) == ESP_OK &&
         storage_path(name, path, pn);
}

static void file_worker(void *) {
  static constexpr size_t CHUNK = 8192;
  char *buf = (char *)malloc(CHUNK);
  for (;;) {
    httpd_req_t *req;
    xQueueReceive(_file_q, &req, portMAX_DELAY);
    char name[24], path[40], qs[48] = {}, disp[64];
    File f;
    if (!buf || !file_name(req, name, sizeof(name), path, sizeof(path)) ||
        !(f = storage_fs().open(path))) {
      httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No such file");
      httpd_req_async_handler_complete(req);
      continue;
    }
    httpd_resp_set_type(req, strstr(name, ".MOV") ? "video/quicktime"
                                                  : "image/jpeg");
    httpd_req_get_url_query_str(req, qs, sizeof(qs));
    if (strstr(qs, "dl=1")) {
      snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", name);
      httpd_resp_set_hdr(req, "Content-Disposition", disp);
    }
    httpd_resp_set_hdr(req, "Cache-Control", "private, max-age=86400");
    size_t n;
    esp_err_t r = ESP_OK;
    while (r == ESP_OK && (n = f.read((uint8_t *)buf, CHUNK)) > 0)
      r = httpd_resp_send_chunk(req, buf, n);
    if (r == ESP_OK)
      httpd_resp_send_chunk(req, nullptr, 0);
    f.close();
    httpd_req_async_handler_complete(req);
  }
}

static esp_err_t file_get_handler(httpd_req_t *req) {
  if (!video_allowed(req))
    return ESP_FAIL;
  char name[24], path[40];
  if (!file_name(req, name, sizeof(name), path, sizeof(path))) {
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No such file");
    return ESP_FAIL;
  }
  httpd_req_t *copy;
  if (!_file_q || httpd_req_async_handler_begin(req, &copy) != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Busy");
    return ESP_FAIL;
  }
  if (xQueueSend(_file_q, &copy, 0) != pdTRUE) {
    httpd_resp_set_status(copy, "503 Service Unavailable");
    httpd_resp_sendstr(copy, "Busy, try again");
    httpd_req_async_handler_complete(copy);
  }
  return ESP_OK;
}

// POST /files/delete?name=IMG_00012.JPG (or name=all)
static esp_err_t file_delete_handler(httpd_req_t *req) {
  if (!authorized(req))
    return ESP_FAIL;
  char qs[48] = {}, name[24] = {};
  if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) != ESP_OK ||
      httpd_query_key_value(qs, "name", name, sizeof(name)) != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "name");
    return ESP_FAIL;
  }
  if (!strcmp(name, "all")) {
    storage_delete_all();
  } else if (!storage_delete(name)) {
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No such file");
    return ESP_FAIL;
  }
  httpd_resp_sendstr(req, "OK");
  return ESP_OK;
}

static esp_err_t photo_handler(httpd_req_t *req) {
  if (!authorized(req))
    return ESP_FAIL;
  if (via_hotspot(req) || !recorder_photo()) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                        "No memory card ready (or on the camera's own WiFi)");
    return ESP_FAIL;
  }
  httpd_resp_sendstr(req, "OK");
  return ESP_OK;
}

// POST /capture/record?do=start|stop
static esp_err_t record_handler(httpd_req_t *req) {
  if (!authorized(req))
    return ESP_FAIL;
  char qs[24] = {}, act[8] = {};
  httpd_req_get_url_query_str(req, qs, sizeof(qs));
  httpd_query_key_value(qs, "do", act, sizeof(act));
  const bool start = !strcmp(act, "start");
  if ((!start && strcmp(act, "stop")) || (start && via_hotspot(req)) ||
      !recorder_record(start)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                        "No memory card ready, or already recording");
    return ESP_FAIL;
  }
  httpd_resp_sendstr(req, "OK");
  return ESP_OK;
}

// ── Animal detection: results, model upload
// ─────────────────────────────────────────────────────────────────
static esp_err_t detect_handler(httpd_req_t *req) {
  if (!video_allowed(req))
    return ESP_FAIL;
  static constexpr size_t BUF = 4096; // up to 64 labels
  char *buf = (char *)malloc(BUF);
  if (!buf) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Out of memory");
    return ESP_FAIL;
  }
  const esp_err_t r = send_json(req, buf, detect_json(buf, BUF));
  free(buf);
  return r;
}

// POST /model?name=&model_len=&labels_len=&range= — body: the .tflite, then
// the labels text. Writes flash as it arrives, like /ota.
static esp_err_t model_handler(httpd_req_t *req) {
  if (!authorized(req))
    return ESP_FAIL;
  char qs[160] = {}, name[64] = "model";
  httpd_req_get_url_query_str(req, qs, sizeof(qs));
  httpd_query_key_value(qs, "name", name, sizeof(name));
  url_decode(name);
  const int ml = query_int(qs, "model_len", 0);
  const int ll = query_int(qs, "labels_len", 0);
  const char *why = "Lengths don't match the upload";
  if (ml <= 0 || ll < 0 || req->content_len != (size_t)(ml + ll) ||
      !detect_upload_begin(name, ml, ll, query_int(qs, "range", 0), &why)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, why);
    return ESP_FAIL;
  }
  static constexpr size_t CHUNK = 4096;
  uint8_t *buf = (uint8_t *)malloc(CHUNK);
  size_t left = req->content_len;
  int timeouts = 0;
  bool ok = buf != nullptr;
  while (ok && left) {
    const int r = httpd_req_recv(req, (char *)buf, left < CHUNK ? left : CHUNK);
    if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 3)
      continue;
    ok = r > 0 && detect_upload_write(buf, r);
    if (ok)
      left -= r;
    timeouts = 0;
  }
  free(buf);
  if (!detect_upload_end(&why) || !ok) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        ok ? why : "The upload was cut off");
    return ESP_FAIL;
  }
  httpd_resp_sendstr(req, "OK");
  return ESP_OK;
}

static esp_err_t model_forget_handler(httpd_req_t *req) {
  if (!authorized(req))
    return ESP_FAIL;
  detect_forget();
  httpd_resp_sendstr(req, "OK");
  return ESP_OK;
}

// POST /system/sleep — now, if a way to wake it is set up
static esp_err_t sleep_handler(httpd_req_t *req) {
  if (!authorized(req))
    return ESP_FAIL;
  const Options &o = options();
  if (!imu_present() || o.imu_int < 0) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                        "It couldn't wake up: needs the motion sensor and its "
                        "wake wire");
    return ESP_FAIL;
  }
  httpd_resp_sendstr(req, "OK");
  system_sleep_in(500);
  return ESP_OK;
}

// ── Every route goes through guarded(): a request whose Host isn't the
// camera gets the captive-portal redirect instead. That also shuts out DNS
// rebinding (a web page whose own name is made to point at the camera).
struct Route {
  const char *uri;
  httpd_method_t method;
  esp_err_t (*fn)(httpd_req_t *);
};

static esp_err_t guarded(httpd_req_t *req) {
  if (!host_ok(req))
    return redirect_home(req);
  // Keeps it awake — except the status polls of a page in a background tab
  if (!strstr(req->uri, "bg=1"))
    power_activity();
  return static_cast<const Route *>(req->user_ctx)->fn(req);
}

static const Route ROUTES[] = {
    {"/", HTTP_GET, index_handler},
    {"/stream", HTTP_GET, stream_redirect_handler},
    {"/pose", HTTP_GET, pose_handler},
    {"/imu/compass", HTTP_POST, compass_handler},
    {"/wifi/scan", HTTP_GET, wifi_scan_handler},
    {"/wifi/connect", HTTP_POST, wifi_connect_handler},
    {"/wifi/hostname", HTTP_POST, hostname_handler},
    {"/ota", HTTP_POST, ota_handler},
    {"/camera/set", HTTP_POST, camera_set_handler},
    {"/snapshot", HTTP_GET, snapshot_handler},
    {"/status", HTTP_GET, status_handler},
    {"/auth", HTTP_GET, auth_handler},
    {"/auth/claim", HTTP_POST, auth_claim_handler},
    {"/auth/forget", HTTP_GET, auth_forget_handler},
    {"/camera/status", HTTP_GET, camera_status_handler},
    {"/camera/reset", HTTP_POST, camera_reset_handler},
    {"/settings/reset", HTTP_POST, settings_reset_handler},
    {"/system/reboot", HTTP_POST, reboot_handler},
    {"/system/sleep", HTTP_POST, sleep_handler},
    {"/options", HTTP_GET, options_handler},
    {"/options/set", HTTP_POST, options_set_handler},
    {"/files", HTTP_GET, files_handler},
    {"/files/get", HTTP_GET, file_get_handler},
    {"/files/delete", HTTP_POST, file_delete_handler},
    {"/capture/photo", HTTP_POST, photo_handler},
    {"/capture/record", HTTP_POST, record_handler},
    {"/detect", HTTP_GET, detect_handler},
    {"/model", HTTP_POST, model_handler},
    {"/model/forget", HTTP_POST, model_forget_handler},
};

// The MJPEG stream: home network and password only, like /snapshot
static esp_err_t stream_guarded(httpd_req_t *req) {
  if (!host_ok(req))
    return redirect_home(req);
  if (!video_allowed(req))
    return ESP_FAIL;
  power_activity();
  return stream_handler(req);
}

bool http_running() { return server && stream_server; }

void http_start() {
  // Control server: dashboard, JSON polling, settings, OTA
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = HTTP_PORT;
  cfg.core_id = NET_CORE;
  cfg.task_priority = PRIO_HTTP_CTRL;
  cfg.stack_size = 8192; // default 4096 is too small for OTA + JSON buffers
  cfg.max_uri_handlers = 32;
  cfg.max_open_sockets = 6;
  cfg.lru_purge_enable = true; // drop oldest idle socket if limit is reached
  cfg.recv_wait_timeout = 30;  // seconds — OTA uploads can be large
  cfg.send_wait_timeout = 10;

  // Stream server: its handler never returns while a client watches, so it
  // gets its own task. One viewer at a time; the next waits for a free slot.
  httpd_config_t scfg = HTTPD_DEFAULT_CONFIG();
  scfg.server_port = STREAM_PORT;
  scfg.ctrl_port = cfg.ctrl_port + 1;
  scfg.core_id = NET_CORE;
  scfg.task_priority = PRIO_HTTP_STREAM;
  scfg.max_uri_handlers = 1;
  scfg.max_open_sockets =
      3; // 6 + 3 + 2×(listen + ctrl) ≤ LWIP_MAX_SOCKETS (16)
  scfg.lru_purge_enable = true;
  scfg.send_wait_timeout = 5; // drop a vanished viewer quickly

  if (httpd_start(&server, &cfg) != ESP_OK) {
    Serial.println("[http] control server failed to start");
    server = nullptr;
    return;
  }
  _file_q = xQueueCreate(8, sizeof(httpd_req_t *));
  xTaskCreatePinnedToCore(file_worker, "files", 4096, nullptr, PRIO_HTTP_STREAM,
                          nullptr, NET_CORE);
  for (const Route &r : ROUTES) {
    const httpd_uri_t u = {r.uri, r.method, guarded, (void *)&r};
    httpd_register_uri_handler(server, &u);
  }
  httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, not_found_handler);

  if (httpd_start(&stream_server, &scfg) != ESP_OK) {
    Serial.println("[http] stream server failed to start");
    stream_server = nullptr;
    return;
  }
  const httpd_uri_t stream = {"/stream", HTTP_GET, stream_guarded, nullptr};
  httpd_register_uri_handler(stream_server, &stream);
  Serial.printf("[http] control :%d, stream :%d\n", HTTP_PORT, STREAM_PORT);
}
