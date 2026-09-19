#include "http_server.h"
#include "auth.h"
#include "calibration_json.h"
#include "calibration_store.h"
#include "camera.h"
#include "camera_profile.h"
#include "config.h"
#include "detections_json.h"
#include "detector.h"
#include "focus.h"
#include "generated/version.h"
#include "generated/web_index.h"
#include "http_util.h"
#include "imu.h"
#include "led.h"
#include "ota_health.h"
#include "push.h"
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
#include <esp_timer.h>

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

// ── /detections
// ─────────────────────────────────────────────────────────────────
static esp_err_t detections_handler(httpd_req_t *req) {
  const DetectionFrame f = detector_latest();
  char buf[3072]; // 8 tags × ~300 B
  const DetectionsJson a = {true, camera_calibrated(calibration_get()),
                            TAG_SIZE_M, millis(), esp_timer_get_time()};
  const size_t n = detections_json(f, a, buf, sizeof(buf));
  return send_json(req, buf, n);
}

// ── /pose
// ───────────────────────────────────────────────────────────────────────
static esp_err_t pose_handler(httpd_req_t *req) {
  double q[4];
  imu_get_quat(q);
  uint32_t n, err;
  const char *state = imu_state(&n, &err);
  char buf[160];
  int len = snprintf(buf, sizeof(buf),
                     "{\"q\":[%.6f,%.6f,%.6f,%.6f],\"imu\":\"%s\",\"n\":%lu,"
                     "\"err\":%lu}",
                     q[0], q[1], q[2], q[3], state, (unsigned long)n,
                     (unsigned long)err);
  httpd_resp_set_type(req, "application/json");
  httpd_resp_send(req, buf, len);
  return ESP_OK;
}

// ── /snapshot, /debug/gray.bmp
// ─────────────────────────────────────────────────────────────────
static esp_err_t snapshot_handler(httpd_req_t *req) {
  // Always at the stream's size, even if the sensor idles at the detection size
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

static esp_err_t gray_handler(httpd_req_t *req) {
  uint8_t *bmp = nullptr;
  size_t len = detector_gray_bmp(&bmp);
  if (!len) {
    httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "No frame processed yet");
    return ESP_FAIL;
  }
  httpd_resp_set_type(req, "image/bmp");
  esp_err_t res = httpd_resp_send(req, (const char *)bmp, len);
  free(bmp);
  return res;
}

// ── /tag?id=N — print page for a tag in the family the detector reads.
// Centred with zero page margins, so A4 and Letter both leave more than the
// one-cell white border tag36h11 needs around the black square.
static esp_err_t tag_handler(httpd_req_t *req) {
  char query[32], val[8];
  int id = 0;
  if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
      httpd_query_key_value(query, "id", val, sizeof(val)) == ESP_OK)
    id = atoi(val);
  if (!detector_ready()) {
    httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR,
                        "Detector not initialised");
    return ESP_FAIL;
  }
  char svg[1536];
  const size_t len = detector_tag_svg(id, svg, sizeof(svg));
  if (!len) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "tag36h11 ids are 0-586");
    return ESP_FAIL;
  }
  char head[512], tail[160];
  snprintf(head, sizeof(head),
           "<!doctype html><meta charset=utf-8><title>tag36h11 #%d</title>"
           "<style>@page{margin:0}html,body{margin:0;background:#fff;"
           "color:#666;font:12px system-ui,sans-serif}body{height:100vh;"
           "display:flex;align-items:center;justify-content:center}"
           "p{position:fixed;bottom:12mm;margin:0}</style>",
           id);
  snprintf(tail, sizeof(tail),
           "<p>AprilTag 36h11 &middot; ID %d &middot; black square %.0f mm "
           "&middot; print at 100%% scale</p>",
           id, TAG_SIZE_M * 1000);
  httpd_resp_set_type(req, "text/html");
  httpd_resp_send_chunk(req, head, HTTPD_RESP_USE_STRLEN);
  httpd_resp_send_chunk(req, svg, len);
  httpd_resp_send_chunk(req, tail, HTTPD_RESP_USE_STRLEN);
  return httpd_resp_send_chunk(req, nullptr, 0);
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
  char buf[1400];
  size_t pos = 0;
  const DetectionFrame f = detector_latest();
  uint32_t imu_n, imu_err;
  const char *imu = imu_state(&imu_n, &imu_err);
  const bool sta = WiFi.status() == WL_CONNECTED;
  char ap_ssid[68], sta_ssid[68], saved[68];
  json_esc(AP_SSID, ap_ssid, sizeof(ap_ssid));
  json_esc(sta ? WiFi.SSID().c_str() : "", sta_ssid, sizeof(sta_ssid));
  json_esc(wifi_saved_ssid(), saved, sizeof(saved));

  appendf(buf, sizeof(buf), pos,
          "{\"uptime\":%lu,\"version\":\"%s\",\"build\":\"%s "
          "%s\",\"heap\":%u,\"psram\":%u,"
          "\"psram_total\":%u,\"temp_c\":%.1f,\"stream_port\":%d,\"auth\":%s,"
          "\"sensor\":\"%s\",\"af\":\"%s\",\"hostname\":\"%s\","
          "\"schema\":%d,\"ota_pending\":%s,",
          (unsigned long)(millis() / 1000), FW_VERSION, __DATE__, __TIME__,
          (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram(),
          (unsigned)ESP.getPsramSize(), temperatureRead(), STREAM_PORT,
          auth_configured() ? "true" : "false", camera_sensor_name(),
          focus_state(), HOSTNAME, settings_schema(),
          ota_health_pending() ? "true" : "false");
  appendf(buf, sizeof(buf), pos,
          "\"imu\":{\"state\":\"%s\",\"samples\":%lu,\"errors\":%lu},", imu,
          (unsigned long)imu_n, (unsigned long)imu_err);
  appendf(buf, sizeof(buf), pos,
          "\"detect\":{\"ready\":%s,\"enabled\":%s,\"tag_mm\":%.0f,"
          "\"calibrated\":%s,\"calibration\":\"%s\",\"res\":%d,"
          "\"push\":{\"port\":%d,\"subscribers\":%d},\"decode_ms\":%u,"
          "\"detect_ms\":%u,"
          "\"age_ms\":%ld,\"count\":%d},",
          detector_ready() ? "true" : "false",
          detector_enabled() ? "true" : "false", TAG_SIZE_M * 1000,
          camera_calibrated(calibration_get()) ? "true" : "false",
          calibration_source(), camera_detect_size(), PUSH_PORT,
          push_subscribers(), f.decode_ms, f.detect_ms,
          f.ms ? (long)(millis() - f.ms) : -1L, f.count);
  appendf(
      buf, sizeof(buf), pos,
      "\"wifi\":{\"ap_ssid\":\"%s\",\"ap_ip\":\"%s\",\"ap_clients\":%d,"
      "\"sta_ssid\":\"%s\",\"sta_ip\":\"%s\",\"rssi\":%d,\"saved\":\"%s\"}}",
      ap_ssid, WiFi.softAPIP().toString().c_str(), WiFi.softAPgetStationNum(),
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

// ── /settings/reset?scope=all|camera|detector|wifi|calibration
// ─────────────────────────────────────────────────────────────────────
static esp_err_t settings_reset_handler(httpd_req_t *req) {
  if (!authorized(req))
    return ESP_FAIL;
  char qs[32] = {}, scope[16] = {};
  if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) != ESP_OK ||
      httpd_query_key_value(qs, "scope", scope, sizeof(scope)) != ESP_OK ||
      !settings_reset(scope)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                        "scope = all|camera|detector|wifi|calibration|token");
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

// ── /calibration — GET the one in use; POST (JSON) sets it; DELETE forgets it
// ─────────────────────────────────────────────────────────────────
static esp_err_t calibration_get_handler(httpd_req_t *req) {
  char buf[400];
  return send_json(req, buf, calibration_json(buf, sizeof(buf)));
}

static esp_err_t calibration_post_handler(httpd_req_t *req) {
  if (!authorized(req))
    return ESP_FAIL;
  char body[512];
  if (!read_body(req, body, sizeof(body)))
    return ESP_FAIL;
  Calibration c;
  if (!calibration_from_json(body, c) || !calibration_set(c)) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST,
                        "Need width, height, fx, fy, cx, cy (+ dist[5]) "
                        "describing a plausible camera");
    return ESP_FAIL;
  }
  Serial.printf("[calib] stored: %dx%d fx=%.1f fy=%.1f cx=%.1f cy=%.1f\n",
                c.width, c.height, c.fx, c.fy, c.cx, c.cy);
  return calibration_get_handler(req);
}

static esp_err_t calibration_delete_handler(httpd_req_t *req) {
  if (!authorized(req))
    return ESP_FAIL;
  calibration_clear();
  Serial.printf("[calib] cleared — %s\n", calibration_source());
  return calibration_get_handler(req);
}

// ── /detector/set?enabled=0|1
// ─────────────────────────────────────────────────────────────────
static esp_err_t detector_set_handler(httpd_req_t *req) {
  if (!authorized(req))
    return ESP_FAIL;
  char qs[48] = {}, val[8] = {};
  if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) != ESP_OK) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing enabled or res");
    return ESP_FAIL;
  }
  bool any = false;
  if (httpd_query_key_value(qs, "enabled", val, sizeof(val)) == ESP_OK) {
    detector_set_enabled(atoi(val) != 0);
    camera_profile_update();
    any = true;
  }
  // res = framesize_t the sensor drops to while nobody watches the stream;
  // 0 = detect at the stream size
  if (httpd_query_key_value(qs, "res", val, sizeof(val)) == ESP_OK) {
    if (!camera_set_detect_size(atoi(val))) {
      httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Bad res");
      return ESP_FAIL;
    }
    any = true;
  }
  if (!any) {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Missing enabled or res");
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
      3; // 6 + 3 + 2×(listen + ctrl) + UDP push ≤ LWIP_MAX_SOCKETS (16)
  scfg.lru_purge_enable = true;
  scfg.send_wait_timeout = 5; // drop a vanished viewer quickly

  if (httpd_start(&server, &cfg) != ESP_OK) {
    Serial.println("[http] control server failed to start");
    return;
  }
  const httpd_uri_t routes[] = {
      {"/", HTTP_GET, index_handler, nullptr},
      {"/stream", HTTP_GET, stream_redirect_handler, nullptr},
      {"/detections", HTTP_GET, detections_handler, nullptr},
      {"/pose", HTTP_GET, pose_handler, nullptr},
      {"/wifi/scan", HTTP_GET, wifi_scan_handler, nullptr},
      {"/wifi/connect", HTTP_POST, wifi_connect_handler, nullptr},
      {"/ota", HTTP_POST, ota_handler, nullptr},
      {"/camera/set", HTTP_POST, camera_set_handler, nullptr},
      {"/snapshot", HTTP_GET, snapshot_handler, nullptr},
      {"/debug/gray.bmp", HTTP_GET, gray_handler, nullptr},
      {"/tag", HTTP_GET, tag_handler, nullptr},
      {"/calibration", HTTP_GET, calibration_get_handler, nullptr},
      {"/calibration", HTTP_POST, calibration_post_handler, nullptr},
      {"/calibration", HTTP_DELETE, calibration_delete_handler, nullptr},
      {"/detector/set", HTTP_POST, detector_set_handler, nullptr},
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

  if (httpd_start(&stream_server, &scfg) != ESP_OK) {
    Serial.println("[http] stream server failed to start");
    return;
  }
  const httpd_uri_t stream = {"/stream", HTTP_GET, stream_handler, nullptr};
  httpd_register_uri_handler(stream_server, &stream);
  Serial.printf("[http] control :%d, stream :%d\n", HTTP_PORT, STREAM_PORT);
}
