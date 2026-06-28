#include "detector.h"
#include "config.h"
#include <Arduino.h>
#include <esp_camera.h>
#include <esp_heap_caps.h>
#include "img_converters.h"
#include "apriltag.h"
#include "tag36h11.h"
#include "apriltag_pose.h"

// Approximate SVGA intrinsics — good enough for pose sign/magnitude;
// calibrate with a checkerboard for sub-cm accuracy.
static constexpr double FX = 800.0;
static constexpr double FY = 800.0;
static constexpr double CX = 400.0;
static constexpr double CY = 300.0;

// Decode JPEG at 1/4 scale: 800×600 → 200×150
static constexpr int DW = 200;
static constexpr int DH = 150;

static apriltag_family_t   *_family    = nullptr;
static apriltag_detector_t *_detector  = nullptr;
static uint8_t             *_rgb_buf   = nullptr;  // DW×DH×2  RGB565
static uint8_t             *_gray_buf  = nullptr;  // DW×DH    grayscale
static uint8_t             *_jpeg_copy = nullptr;  // scratch for JPEG copy
static constexpr size_t     JPEG_BUF   = 65536;   // 64 KB — covers SVGA q12

static DetectionFrame _latest  = {};
static bool           _enabled = true;

void detector_init() {
    _family   = tag36h11_create();
    _detector = apriltag_detector_create();
    apriltag_detector_add_family(_detector, _family);
    _detector->quad_decimate = 1.0f;  // already low-res, no further decimation
    _detector->quad_sigma    = 0.0f;
    _detector->refine_edges  = 1;
    _detector->nthreads      = 1;

    _rgb_buf   = (uint8_t *)heap_caps_malloc(DW * DH * 2, MALLOC_CAP_SPIRAM);
    _gray_buf  = (uint8_t *)heap_caps_malloc(DW * DH,     MALLOC_CAP_SPIRAM);
    _jpeg_copy = (uint8_t *)heap_caps_malloc(JPEG_BUF,    MALLOC_CAP_SPIRAM);

    if (!_rgb_buf || !_gray_buf || !_jpeg_copy)
        Serial.println("[detect] PSRAM alloc failed");
    else
        Serial.println("[detect] AprilTag (tag36h11) ready");
}

static void run(const uint8_t *jpeg, size_t len) {
    // Decode JPEG to RGB565 at 1/4 scale → 200×150
    if (!jpg2rgb565(jpeg, len, _rgb_buf, JPG_SCALE_4X)) {
        Serial.println("[detect] JPEG decode failed");
        return;
    }

    // RGB565 → grayscale (luminance)
    uint16_t *src = (uint16_t *)_rgb_buf;
    for (int i = 0; i < DW * DH; i++) {
        uint16_t px = src[i];
        uint8_t r = ((px >> 11) & 0x1F) << 3;
        uint8_t g = ((px >> 5)  & 0x3F) << 2;
        uint8_t b = ( px        & 0x1F) << 3;
        _gray_buf[i] = (uint8_t)((r * 77u + g * 150u + b * 29u) >> 8u);
    }

    image_u8_t im = { DW, DH, DW, _gray_buf };
    zarray_t *dets = apriltag_detector_detect(_detector, &im);

    _latest.count = 0;
    int n = zarray_size(dets);
    Serial.printf("[detect] %d tag(s) found\n", n);

    for (int i = 0; i < n && i < MAX_TAGS; i++) {
        apriltag_detection_t *det;
        zarray_get(dets, i, &det);

        TagDetection &out = _latest.tags[_latest.count++];
        out.id = det->id;

        // Scale corners back to full SVGA coordinates
        for (int c = 0; c < 4; c++) {
            out.corners[c][0] = (float)(det->p[c][0] * 4.0);
            out.corners[c][1] = (float)(det->p[c][1] * 4.0);
        }

        // Pose estimation (rotation ignored for now — translation only on serial)
        apriltag_detection_info_t info = { det, TAG_SIZE_M, FX, FY, CX, CY };
        apriltag_pose_t pose;
        double err = estimate_tag_pose(&info, &pose);
        out.tx = MATD_EL(pose.t, 0, 0);
        out.ty = MATD_EL(pose.t, 1, 0);
        out.tz = MATD_EL(pose.t, 2, 0);
        matd_destroy(pose.R);
        matd_destroy(pose.t);

        Serial.printf("[detect] tag %3d  t=[%+.3f %+.3f %+.3f] m  err=%.4f\n",
                      out.id, out.tx, out.ty, out.tz, err);
    }

    apriltag_detections_destroy(dets);
}

void detector_toggle() {
    _enabled = !_enabled;
    Serial.printf("[detect] %s\n", _enabled ? "enabled" : "disabled");
}

void detector_grab() {
    if (!_enabled || !_jpeg_copy || !_detector) return;

    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) return;

    // Copy JPEG and return the camera buffer immediately so the stream isn't stalled
    size_t len = fb->len;
    if (len > JPEG_BUF) { esp_camera_fb_return(fb); return; }
    memcpy(_jpeg_copy, fb->buf, len);
    esp_camera_fb_return(fb);

    Serial.printf("[cam] %ux%u  %u bytes\n", fb->width, fb->height, len);
    run(_jpeg_copy, len);
}

const DetectionFrame& detector_latest() { return _latest; }
