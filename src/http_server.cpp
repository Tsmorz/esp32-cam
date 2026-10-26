#include "http_server.h"
#include "auth.h"
#include "camera.h"
#include "camera_profile.h"
#include "config.h"
#include "focus.h"
#include "generated/version.h"
#include "generated/web_index.h"
#include "http_util.h"
#include "imu.h"
#include "led.h"
#include "ota_health.h"
#include "settings.h"
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
#include <lwip/sockets.h>

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
// itself still get a 404.
static esp_err_t not_found_handler(httpd_req_t *req, httpd_err_code_t) {
  char host[64] = "";
  httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host));
  char *colon = strchr(host, ':');
  if (colon)
    *colon = '\0';
  // The address this request arrived on (hotspot or home network)
  char self[INET_ADDRSTRLEN] = "";
  struct sockaddr_storage sa;
  socklen_t len = sizeof(sa);
  if (getsockname(httpd_req_to_sockfd(req), (struct sockaddr *)&sa, &len) ==
      0) {
    if (sa.ss_family == AF_INET)
      inet_ntop(AF_INET, &((struct sockaddr_in *)&sa)->sin_addr, self,
                sizeof(self));
    else if (sa.ss_family == AF_INET6) // IPv4-mapped: the last 4 bytes
      inet_ntop(AF_INET,
                &((struct sockaddr_in6 *)&sa)->sin6_addr.un.u32_addr[3], self,
                sizeof(self));
  }
  const bool ours = !host[0] || !strcmp(host, self) ||
                    !strcasecmp(host, HOSTNAME ".local") ||
                    !strcmp(host, WiFi.softAPIP().toString().c_str()) ||
                    !strcmp(host, WiFi.localIP().toString().c_str());
  if (ours || !self[0]) {
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "Not found");
    return ESP_FAIL;
  }
  char loc[48];
  snprintf(loc, sizeof(loc), "http://%s/", self);
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", loc);
  httpd_resp_set_hdr(req, "Cache-Control", "no-store");
  httpd_resp_send(req, nullptr, 0);
  return ESP_OK;
}

// ── /pose — orientation from the IMU
// ─────────────────────────────────────────────────────────────────
static esp_err_t pose_handler(httpd_req_t *req) {
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
  char buf[1024];
  size_t pos = 0;
  const sensor_t *sen = esp_camera_sensor_get();
  const bool sta = WiFi.status() == WL_CONNECTED;
  char ap_ssid[68], sta_ssid[68], saved[68];
  json_esc(AP_SSID, ap_ssid, sizeof(ap_ssid));
  json_esc(sta ? WiFi.SSID().c_str() : "", sta_ssid, sizeof(sta_ssid));
  json_esc(wifi_saved_ssid(), saved, sizeof(saved));
  uint32_t imu_n, imu_err;
  const char *imu = imu_state(&imu_n, &imu_err);

  appendf(buf, sizeof(buf), pos,
          "{\"uptime\":%lu,\"version\":\"%s\",\"build\":\"%s "
          "%s\",\"heap\":%u,\"psram\":%u,"
          "\"psram_total\":%u,\"temp_c\":%.1f,\"stream_port\":%d,\"auth\":%s,"
          "\"mirror\":%s,"
          "\"sensor\":\"%s\",\"af\":\"%s\",\"hostname\":\"%s\","
          "\"schema\":%d,\"ota_pending\":%s,",
          (unsigned long)(millis() / 1000), FW_VERSION, __DATE__, __TIME__,
          (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram(),
          (unsigned)ESP.getPsramSize(), temperatureRead(), STREAM_PORT,
          auth_configured() ? "true" : "false",
          sen && sen->status.hmirror ? "true" : "false", camera_sensor_name(),
          focus_state(), HOSTNAME, settings_schema(),
          ota_health_pending() ? "true" : "false");
  appendf(buf, sizeof(buf), pos,
          "\"imu\":{\"state\":\"%s\",\"samples\":%lu,\"errors\":%lu,"
          "\"mag\":\"%s\"},",
          imu, (unsigned long)imu_n, (unsigned long)imu_err, imu_mag_state());
  // ap_on: wifi_tick() turns the hotspot off while STA is up and nobody uses it
  appendf(buf, sizeof(buf), pos,
          "\"wifi\":{\"ap_on\":%s,\"ap_ssid\":\"%s\",\"ap_ip\":\"%s\","
          "\"ap_clients\":%d,\"sta_ssid\":\"%s\",\"sta_ip\":\"%s\",\"rssi\":%d,"
          "\"saved\":\"%s\"}}",
          (WiFi.getMode() & WIFI_AP) ? "true" : "false", ap_ssid,
          WiFi.softAPIP().toString().c_str(), WiFi.softAPgetStationNum(),
          sta_ssid, sta ? WiFi.localIP().toString().c_str() : "",
          sta ? WiFi.RSSI() : 0, saved);

  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, buf, pos);
  return ESP_OK;
}

// ── /camera/status — current sensor settings, to populate the Camera page
// ─────────────────────────────────────────────────────────────────────
static esp_err_t camera_status_handler(httpd_req_t *req) {
  char buf[640];
  size_t len = camera_status_json(buf, sizeof(buf));
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, buf, len);
  return ESP_OK;
}

// ── /auth — is the token right? Lets the dashboard find out before it sends a
// firmware image: a rejected upload comes back to the browser as a dropped
// connection, not as the 401.
static esp_err_t auth_handler(httpd_req_t *req) {
  if (!authorized(req))
    return ESP_FAIL;
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
  switch (auth_claim(tok)) {
  case ClaimResult::Ok:
    httpd_resp_sendstr(req, "OK");
    return ESP_OK;
  case ClaimResult::AlreadySet:
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "A token is already set");
    return ESP_FAIL;
  default:
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                        "Token must be 8-64 printable characters, no spaces");
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
            i > 0 ? "," : "", esc, WiFi.RSSI(i),
            (int)(WiFi.encryptionType(i) != WIFI_AUTH_OPEN));
  }
  appendf(buf, BUF, pos, "]}");
  const esp_err_t res = send_json(req, buf, pos);
  free(buf);
  return res;
}

// ── /wifi/connect
// ────────────────────────────────────────────────────────────────
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

  httpd_resp_sendstr(req, "OK"); // before the link may change under this reply
  wifi_save_and_connect(ssid, pass);
  return ESP_OK;
}

// ── /ota
// ────────────────────────────────────────────────────────────────────────
// Uses ESP-IDF esp_ota_ops directly — avoids Arduino Update.h global
// constructor (UpdateClass Update) which crashes before setup() on OPI PSRAM.
static esp_err_t ota_handler(httpd_req_t *req) {
  if (!authorized(req))
    return ESP_FAIL; // closes the socket instead of draining the upload
  led_set(LED_OTA);
  const esp_partition_t *dst = esp_ota_get_next_update_partition(NULL);
  if (!dst || dst == esp_ota_get_running_partition()) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "No OTA partition — check partition table");
    return ESP_FAIL;
  }
  size_t total = req->content_len;
  esp_ota_handle_t ota;
  if (esp_ota_begin(dst, total > 0 ? total : OTA_SIZE_UNKNOWN, &ota) !=
      ESP_OK) {
    led_set(LED_IDLE);
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "OTA begin failed");
    return ESP_FAIL;
  }
  uint8_t buf[2048];
  size_t remaining = total;
  bool ok = true;
  while (remaining > 0 || total == 0) {
    size_t to_read =
        (remaining > 0 && remaining < sizeof(buf)) ? remaining : sizeof(buf);
    int recv = httpd_req_recv(req, (char *)buf, to_read);
    if (recv == 0)
      break;
    if (recv < 0 || esp_ota_write(ota, buf, recv) != ESP_OK) {
      ok = false;
      break;
    }
    if (total > 0)
      remaining -= recv;
  }
  if (!ok)
    esp_ota_abort(ota);
  if (!ok || esp_ota_end(ota) != ESP_OK ||
      esp_ota_set_boot_partition(dst) != ESP_OK) {
    led_set(LED_IDLE);
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
  char qs[128] = {}, var[32] = {}, val[16] = {};
  if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) != ESP_OK ||
      httpd_query_key_value(qs, "var", var, sizeof(var)) != ESP_OK ||
      httpd_query_key_value(qs, "val", val, sizeof(val)) != ESP_OK ||
      !camera_set(var, atoi(val))) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad var or val");
    return ESP_FAIL;
  }
  httpd_resp_sendstr(req, "OK");
  return ESP_OK;
}

void http_start() {
  // Control server: dashboard, JSON polling, settings, OTA
  httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
  cfg.server_port = HTTP_PORT;
  cfg.core_id = NET_CORE;
  cfg.task_priority = PRIO_HTTP_CTRL;
  cfg.stack_size = 8192; // default 4096 is too small for OTA + JSON buffers
  cfg.max_uri_handlers = 24;
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
    return;
  }
  const httpd_uri_t routes[] = {
      {"/", HTTP_GET, index_handler, nullptr},
      {"/stream", HTTP_GET, stream_redirect_handler, nullptr},
      {"/pose", HTTP_GET, pose_handler, nullptr},
      {"/imu/compass", HTTP_POST, compass_handler, nullptr},
      {"/wifi/scan", HTTP_GET, wifi_scan_handler, nullptr},
      {"/wifi/connect", HTTP_POST, wifi_connect_handler, nullptr},
      {"/ota", HTTP_POST, ota_handler, nullptr},
      {"/camera/set", HTTP_POST, camera_set_handler, nullptr},
      {"/snapshot", HTTP_GET, snapshot_handler, nullptr},
      {"/status", HTTP_GET, status_handler, nullptr},
      {"/auth", HTTP_GET, auth_handler, nullptr},
      {"/auth/claim", HTTP_POST, auth_claim_handler, nullptr},
      {"/camera/status", HTTP_GET, camera_status_handler, nullptr},
      {"/camera/reset", HTTP_POST, camera_reset_handler, nullptr},
      {"/settings/reset", HTTP_POST, settings_reset_handler, nullptr},
      {"/system/reboot", HTTP_POST, reboot_handler, nullptr},
  };
  for (const httpd_uri_t &r : routes)
    httpd_register_uri_handler(server, &r);
  httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, not_found_handler);

  if (httpd_start(&stream_server, &scfg) != ESP_OK) {
    Serial.println("[http] stream server failed to start");
    return;
  }
  const httpd_uri_t stream = {"/stream", HTTP_GET, stream_handler, nullptr};
  httpd_register_uri_handler(stream_server, &stream);
  Serial.printf("[http] control :%d, stream :%d\n", HTTP_PORT, STREAM_PORT);
}
