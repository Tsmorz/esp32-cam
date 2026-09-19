// AprilTag set-up and pose maths, with the real apriltag library.
// Run with: pio test -e native

#include "tag_pose.h"
#include <unity.h>
#include <utility>

static const Calibration NONE = {800, 600, 0, 0, 0, 0, {0, 0, 0, 0, 0}};
static const Calibration CAL = {
    800, 600, 812.5, 810.0, 405.0, 297.0, {-0.28, 0.09, 0.001, -0.0015, 0.0}};
static constexpr double SIZE = 0.165; // tag black square, metres

static apriltag_family_t *fam;
static apriltag_detector_t *td;

void setUp() {
  fam = tag36h11_create();
  td = apriltag_detector_create();
  configure_detector(td, fam);
}

void tearDown() {
  apriltag_detector_destroy(td);
  tag36h11_destroy(fam);
}

// White w×h image with tag `id` drawn `cell` px per grid cell, its black
// square centred on (cx, cy)
static image_u8_t *render(int id, int w, int h, int cell, int cx, int cy) {
  image_u8_t *tag = apriltag_to_image(fam, id);
  image_u8_t *im = image_u8_create(w, h);
  memset(im->buf, 255, im->stride * h);
  const int x0 = cx - tag->width * cell / 2, y0 = cy - tag->height * cell / 2;
  for (int y = 0; y < tag->height * cell; y++)
    for (int x = 0; x < tag->width * cell; x++)
      im->buf[(y0 + y) * im->stride + x0 + x] =
          tag->buf[(y / cell) * tag->stride + x / cell];
  image_u8_destroy(tag);
  return im;
}

// Detect exactly one tag in a 400×300 frame (what the firmware decodes an
// 800×600 stream to, div = 2), black square centred on (cx, cy)
static apriltag_detection_t *detect_one(zarray_t **dets, int id, int cell,
                                        int cx = 200, int cy = 150) {
  image_u8_t *im = render(id, 400, 300, cell, cx, cy);
  *dets = apriltag_detector_detect(td, im);
  image_u8_destroy(im);
  TEST_ASSERT_EQUAL_INT(1, zarray_size(*dets));
  apriltag_detection_t *det;
  zarray_get(*dets, 0, &det);
  TEST_ASSERT_EQUAL_INT(id, det->id);
  return det;
}

// Move `ideal` corners (decoded pixels) through the lens model into det->p,
// as a real lens would — OpenCV's forward model, independent of the code
// under test
static void distort_corners(apriltag_detection_t *det, const double ideal[4][2],
                            const CameraModel &cam, int div) {
  const double *d = cam.dist;
  for (int i = 0; i < 4; i++) {
    const double x = (ideal[i][0] * div - cam.cx) / cam.fx;
    const double y = (ideal[i][1] * div - cam.cy) / cam.fy;
    const double r2 = x * x + y * y;
    const double rad = 1 + r2 * (d[0] + r2 * (d[1] + r2 * d[4]));
    const double xd = x * rad + 2 * d[2] * x * y + d[3] * (r2 + 2 * x * x);
    const double yd = y * rad + d[2] * (r2 + 2 * y * y) + 2 * d[3] * x * y;
    det->p[i][0] = (xd * cam.fx + cam.cx) / div;
    det->p[i][1] = (yd * cam.fy + cam.cy) / div;
  }
}

static double quat_angle_deg(const float a[4], const float b[4]) {
  const double dot =
      fabs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]);
  return 2 * acos(fmin(1.0, dot)) * 180 / M_PI;
}

void test_decode_table_fits_in_psram() {
  TEST_ASSERT_NOT_NULL(fam->impl);
  // apriltag's struct quick_decode starts with its entry count; 16 B each.
  // The 2-bit default would need ~36 MB — the bug that stopped all detection.
  const long bytes = (long)*(const int *)fam->impl * 16;
  TEST_ASSERT_LESS_THAN_INT32(2 * 1024 * 1024, bytes);
}

void test_self_test_detects_its_tag() {
  TEST_ASSERT_TRUE(detector_self_test(td, fam));
}

void test_head_on_tag_pose() {
  zarray_t *dets;
  apriltag_detection_t *det = detect_one(&dets, 42, 10); // square 80 px
  const TagPose p = tag_pose(det, camera_model(800, 600, NONE), 2, SIZE);
  // fx = 400 in decoded pixels: z = fx · size / width = 400 · 0.165 / 80
  TEST_ASSERT_DOUBLE_WITHIN(0.01, 0.825, p.t[2]);
  TEST_ASSERT_DOUBLE_WITHIN(0.005, 0, p.t[0]);
  TEST_ASSERT_DOUBLE_WITHIN(0.005, 0, p.t[1]);
  // Facing the camera squarely, rotation is barely observable: 1° of tilt
  // moves this 80 px tag's corners by < 0.1 px. Measured ≈ 1.1°.
  const float identity[4] = {1, 0, 0, 0};
  TEST_ASSERT_LESS_THAN_DOUBLE(3.0, quat_angle_deg(p.q, identity));
  apriltag_detections_destroy(dets);
}

void test_undistort_detection_recovers_corners_and_homography() {
  zarray_t *dets;
  // Off-centre, where the lens moves the corners by several pixels
  apriltag_detection_t *det = detect_one(&dets, 7, 10, 330, 220);
  const CameraModel cam = camera_model(800, 600, CAL);
  double ideal[4][2];
  memcpy(ideal, det->p, sizeof(ideal));
  distort_corners(det, ideal, cam, 2);
  TEST_ASSERT_TRUE(fabs(det->p[2][0] - ideal[2][0]) > 2); // really moved
  undistort_detection(det, cam, 2);
  for (int i = 0; i < 4; i++) {
    TEST_ASSERT_DOUBLE_WITHIN(1e-3, ideal[i][0], det->p[i][0]);
    TEST_ASSERT_DOUBLE_WITHIN(1e-3, ideal[i][1], det->p[i][1]);
    // The rebuilt homography maps tag corner i onto the undistorted corner
    double u, v;
    homography_project(det->H, (i == 1 || i == 2) ? 1 : -1, i < 2 ? 1 : -1, &u,
                       &v);
    TEST_ASSERT_DOUBLE_WITHIN(1e-2, ideal[i][0], u);
    TEST_ASSERT_DOUBLE_WITHIN(1e-2, ideal[i][1], v);
  }
  apriltag_detections_destroy(dets);
}

void test_pose_with_distortion_matches_ideal_camera() {
  // Truth: the undistorted image seen through the same fx/fy/cx/cy
  Calibration pinhole = CAL;
  memset(pinhole.dist, 0, sizeof(pinhole.dist));
  zarray_t *dets;
  apriltag_detection_t *det = detect_one(&dets, 3, 10, 330, 220);
  double ideal[4][2];
  memcpy(ideal, det->p, sizeof(ideal));
  const TagPose truth = tag_pose(det, camera_model(800, 600, pinhole), 2, SIZE);

  // Same tag, corners distorted by the lens, then the firmware's pose path
  const CameraModel cam = camera_model(800, 600, CAL);
  distort_corners(det, ideal, cam, 2);
  const TagPose p = tag_pose(det, cam, 2, SIZE);
  for (int i = 0; i < 3; i++)
    TEST_ASSERT_DOUBLE_WITHIN(0.001, truth.t[i], p.t[i]);
  TEST_ASSERT_LESS_THAN_DOUBLE(0.1, quat_angle_deg(p.q, truth.q));

  // …and ignoring the distortion really would have been wrong
  distort_corners(det, ideal, cam, 2);
  const TagPose naive = tag_pose(det, camera_model(800, 600, pinhole), 2, SIZE);
  TEST_ASSERT_TRUE(fabs(naive.t[2] - truth.t[2]) > 0.01);
  apriltag_detections_destroy(dets);
}

static void rotation(double axis_x, double axis_y, double axis_z, double deg,
                     float q_out[4]) {
  const double n = sqrt(axis_x * axis_x + axis_y * axis_y + axis_z * axis_z);
  const double x = axis_x / n, y = axis_y / n, z = axis_z / n;
  const double a = deg * M_PI / 180, c = cos(a), s = sin(a), t = 1 - c;
  double R[9] = {t * x * x + c,     t * x * y - s * z, t * x * z + s * y,
                 t * x * y + s * z, t * y * y + c,     t * y * z - s * x,
                 t * x * z - s * y, t * y * z + s * x, t * z * z + c};
  matd_t *M = matd_create_data(3, 3, R);
  mat_to_quat(M, q_out);
  matd_destroy(M);
}

void test_mat_to_quat() {
  float q[4];
  const float identity[4] = {1, 0, 0, 0};
  rotation(0, 0, 1, 0, q);
  TEST_ASSERT_LESS_THAN_DOUBLE(1e-4, quat_angle_deg(q, identity));

  rotation(0, 0, 1, 90, q); // [cos 45°, 0, 0, sin 45°]
  TEST_ASSERT_FLOAT_WITHIN(1e-6, (float)M_SQRT1_2, q[0]);
  TEST_ASSERT_FLOAT_WITHIN(1e-6, (float)M_SQRT1_2, q[3]);

  rotation(1, 0, 0, 180, q); // trace −1: the w = 0 branch
  TEST_ASSERT_FLOAT_WITHIN(1e-6, 0, q[0]);
  TEST_ASSERT_FLOAT_WITHIN(1e-6, 1, fabsf(q[1]));

  rotation(1, -2, 0.5, 250, q); // > 180°: w flips sign to stay ≥ 0
  const double half = 250 / 2.0 * M_PI / 180, n = sqrt(1 + 4 + 0.25);
  const float expect[4] = {(float)cos(half), (float)(sin(half) / n),
                           (float)(-2 * sin(half) / n),
                           (float)(0.5 * sin(half) / n)};
  TEST_ASSERT_TRUE(q[0] >= 0);
  TEST_ASSERT_LESS_THAN_DOUBLE(1e-3, quat_angle_deg(q, expect));
  TEST_ASSERT_FLOAT_WITHIN(
      1e-6, 1, q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
}

// tag_pose() replaces apriltag's orthogonal iteration (matd_op + malloc per
// step, double) with fixed-size float arrays; it must give apriltag's answer
// at the same step count to float precision. (apriltag's estimate_tag_pose()
// hard-codes 50 steps; this runs its orthogonal iteration with POSE_STEPS.)
static void test_pose_matches_apriltag_estimate() {
  const CameraModel cam = camera_model(800, 600, NONE);
  srand(7);
  auto rnd = [] { return rand() / (double)RAND_MAX; };
  for (int n = 0; n < 200; n++) {
    apriltag_detection_t det = {};
    zarray_t *corr = zarray_create(sizeof(float[4]));
    const double cx = 150 + rnd() * 500, cy = 100 + rnd() * 400,
                 r = 20 + rnd() * 120, a0 = rnd() * 2 * M_PI;
    for (int i = 0; i < 4; i++) { // perturbed square, apriltag corner order
      const double a = a0 - i * M_PI / 2 + (rnd() - 0.5) * 0.5,
                   rr = r * (0.7 + 0.6 * rnd());
      det.p[i][0] = cx + rr * cos(a);
      det.p[i][1] = cy + rr * sin(a);
      const float c[4] = {(i == 1 || i == 2) ? 1.f : -1.f, i < 2 ? 1.f : -1.f,
                          (float)det.p[i][0], (float)det.p[i][1]};
      zarray_add(corr, c);
    }
    det.H = homography_compute(corr, HOMOGRAPHY_COMPUTE_FLAG_SVD);
    zarray_destroy(corr);

    apriltag_detection_info_t info = {&det,   SIZE,   cam.fx,
                                      cam.fy, cam.cx, cam.cy};
    // estimate_tag_pose() with POSE_STEPS instead of its fixed 50
    double err1, err2;
    apriltag_pose_t ref, alt;
    estimate_tag_pose_orthogonal_iteration(&info, &err1, &ref, &err2, &alt,
                                           POSE_STEPS);
    if (err2 < err1)
      std::swap(ref, alt);
    const double ref_err = fmin(err1, err2);
    if (alt.R) { // no second minimum: R null, t never created
      matd_destroy(alt.R);
      matd_destroy(alt.t);
    }
    float ref_q[4];
    mat_to_quat(ref.R, ref_q);

    const TagPose got = tag_pose(&det, cam, 1, SIZE);
    TEST_ASSERT_DOUBLE_WITHIN(1e-9 + 1e-3 * ref_err, ref_err, got.err);
    for (int i = 0; i < 3; i++)
      TEST_ASSERT_DOUBLE_WITHIN(5e-4 * fabs(MATD_EL(ref.t, 2, 0)),
                                MATD_EL(ref.t, i, 0), got.t[i]);
    for (int i = 0; i < 4; i++) // not the angle: acos near 1 magnifies
      TEST_ASSERT_FLOAT_WITHIN(5e-4, ref_q[i], got.q[i]); // float rounding
    matd_destroy(ref.R);
    matd_destroy(ref.t);
    matd_destroy(det.H);
  }
}

int main() {
  UNITY_BEGIN();
  RUN_TEST(test_pose_matches_apriltag_estimate);
  RUN_TEST(test_decode_table_fits_in_psram);
  RUN_TEST(test_self_test_detects_its_tag);
  RUN_TEST(test_head_on_tag_pose);
  RUN_TEST(test_undistort_detection_recovers_corners_and_homography);
  RUN_TEST(test_pose_with_distortion_matches_ideal_camera);
  RUN_TEST(test_mat_to_quat);
  return UNITY_END();
}
