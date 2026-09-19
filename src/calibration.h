#pragma once

// ── Camera calibration ──────────────────────────────────────────────────────
// Put your camera's calibration here, then rebuild and flash. Until CALIB_FX
// is set, tag poses use a rough estimate: fx = fy = the width of the full 4:3
// view, principal point at the centre, no lens distortion.
//
// Values are in pixels at the resolution you calibrated at (CALIB_WIDTH ×
// CALIB_HEIGHT). The firmware rescales them to whatever resolution the camera
// runs at, including 240×240 and 1280×720, which are centred crops of the
// sensor's 4:3 view — so calibrate at a 4:3 size (e.g. 800×600) to cover all.
// Calibrate with the same Mirror/Flip settings you run with.
//
// From OpenCV, cv2.calibrateCamera() → camera matrix K and dist_coeffs:
//   CALIB_FX = K[0][0]   CALIB_FY = K[1][1]
//   CALIB_CX = K[0][2]   CALIB_CY = K[1][2]
//   CALIB_DIST = the first five dist_coeffs: k1, k2, p1, p2, k3

#define CALIB_WIDTH 800 // resolution the calibration was done at
#define CALIB_HEIGHT 600

#define CALIB_FX 0.0 // 0 = not calibrated: use the estimate above
#define CALIB_FY 0.0
#define CALIB_CX 0.0
#define CALIB_CY 0.0

#define CALIB_DIST {0.0, 0.0, 0.0, 0.0, 0.0} // k1, k2, p1, p2, k3
