#pragma once

// AprilTag set-up and pose maths shared by the firmware (detector.cpp) and the
// unit tests (test/). Only apriltag and camera_model.h — no Arduino — so it
// builds on a PC too.

#include "apriltag.h"
#include "apriltag_pose.h"
#include "camera_model.h"
#include "common/homography.h"
#include "tag36h11.h"
#include <math.h>
#include <string.h>

// Correct up to 1 bit error. The library default (2) builds a ~36 MB decode
// table for tag36h11; the allocation fails on 8 MB PSRAM and apriltag then
// silently matches nothing. 1 bit needs ~1 MB.
static constexpr int TAG_BITS_CORRECTED = 1;

// Detector settings for frames decoded at up to 400×300. Returns false if the
// family's decode table could not be allocated.
static inline bool configure_detector(apriltag_detector_t *td,
                                      apriltag_family_t *family) {
  apriltag_detector_add_family_bits(td, family, TAG_BITS_CORRECTED);
  td->quad_decimate = 2.0f; // quads at 200×150, bits at 400×300
  td->quad_sigma = 0.0f;
  // Skip regions with less contrast than this before fitting quads: sensor
  // noise and JPEG artefacts otherwise make hundreds of candidate blobs. On
  // synthetic tags nothing is lost up to 20; at 25 tags with 15 grey levels
  // between black and white stop being found.
  td->qtp.min_white_black_diff = 20;
  td->refine_edges = 1;
  td->nthreads = 1;
  return family->impl != nullptr;
}

// Draw tag 0 on a blank image and detect it. apriltag reports some setup
// failures only by never matching anything, so check before trusting it.
static inline bool detector_self_test(apriltag_detector_t *td,
                                      apriltag_family_t *family) {
  const int cell = 12, margin = 20;
  image_u8_t *tag = apriltag_to_image(family, 0); // includes white border
  const int side = tag->width * cell + 2 * margin;
  image_u8_t *im = image_u8_create(side, side);
  memset(im->buf, 255, im->stride * im->height);
  for (int y = 0; y < tag->height * cell; y++)
    for (int x = 0; x < tag->width * cell; x++)
      im->buf[(margin + y) * im->stride + margin + x] =
          tag->buf[(y / cell) * tag->stride + x / cell];

  zarray_t *dets = apriltag_detector_detect(td, im);
  bool found = false;
  for (int i = 0; i < zarray_size(dets); i++) {
    apriltag_detection_t *det;
    zarray_get(dets, i, &det);
    found |= det->id == 0;
  }
  apriltag_detections_destroy(dets);
  image_u8_destroy(im);
  image_u8_destroy(tag);
  return found;
}

// Undistort a detection's corners (decoded pixels; the model is in source
// pixels, `div` times larger) and rebuild its homography to match, so
// estimate_tag_pose() sees an ideal pinhole camera.
static inline void undistort_detection(apriltag_detection_t *det,
                                       const CameraModel &cam, int div) {
  zarray_t *corr = zarray_create(sizeof(float[4]));
  for (int i = 0; i < 4; i++) {
    double u = det->p[i][0] * div, v = det->p[i][1] * div;
    undistort_point(cam, u, v);
    det->p[i][0] = u / div;
    det->p[i][1] = v / div;
    // Tag-frame corner i in apriltag.c's order: (-1,1) (1,1) (1,-1) (-1,-1)
    const float c[4] = {(i == 1 || i == 2) ? 1.f : -1.f, i < 2 ? 1.f : -1.f,
                        (float)det->p[i][0], (float)det->p[i][1]};
    zarray_add(corr, c);
  }
  matd_t *H = homography_compute(corr, HOMOGRAPHY_COMPUTE_FLAG_SVD);
  zarray_destroy(corr);
  if (H) {
    matd_destroy(det->H);
    det->H = H;
  }
}

// Rotation matrix → unit quaternion [w, x, y, z], w ≥ 0
static inline void mat_to_quat(const matd_t *R, float q[4]) {
  const double m00 = MATD_EL(R, 0, 0), m01 = MATD_EL(R, 0, 1),
               m02 = MATD_EL(R, 0, 2), m10 = MATD_EL(R, 1, 0),
               m11 = MATD_EL(R, 1, 1), m12 = MATD_EL(R, 1, 2),
               m20 = MATD_EL(R, 2, 0), m21 = MATD_EL(R, 2, 1),
               m22 = MATD_EL(R, 2, 2);
  double w, x, y, z;
  const double tr = m00 + m11 + m22;
  if (tr > 0) {
    const double s = sqrt(tr + 1.0) * 2;
    w = 0.25 * s, x = (m21 - m12) / s, y = (m02 - m20) / s, z = (m10 - m01) / s;
  } else if (m00 > m11 && m00 > m22) {
    const double s = sqrt(1.0 + m00 - m11 - m22) * 2;
    w = (m21 - m12) / s, x = 0.25 * s, y = (m01 + m10) / s, z = (m02 + m20) / s;
  } else if (m11 > m22) {
    const double s = sqrt(1.0 + m11 - m00 - m22) * 2;
    w = (m02 - m20) / s, x = (m01 + m10) / s, y = 0.25 * s, z = (m12 + m21) / s;
  } else {
    const double s = sqrt(1.0 + m22 - m00 - m11) * 2;
    w = (m10 - m01) / s, x = (m02 + m20) / s, y = (m12 + m21) / s, z = 0.25 * s;
  }
  const double k = (w < 0 ? -1 : 1) / sqrt(w * w + x * x + y * y + z * z);
  q[0] = w * k, q[1] = x * k, q[2] = y * k, q[3] = z * k;
}

struct TagPose {
  double t[3]; // tag centre in the camera frame, metres
  float q[4];  // tag orientation in the camera frame, [w, x, y, z]
  double err;  // apriltag's object-space error
};

// Orthogonal iteration (Lu, Hager & Mjolsness 2000) for the four corners of a
// planar tag, the same maths as apriltag's orthogonal_iteration() but on
// fixed-size arrays. apriltag's version builds every intermediate matrix with
// matd_op() (string parsing + malloc) and a general SVD; on the ESP32-S3 its
// 2 × 50 steps took ~310 ms per tag.

// Eigen-decomposition of a symmetric 3×3 matrix by cyclic Jacobi rotations.
// On return the diagonal of A holds the eigenvalues, V's columns the vectors.
static inline void sym3_eigen(float A[3][3], float V[3][3]) {
  for (int i = 0; i < 3; i++)
    for (int j = 0; j < 3; j++)
      V[i][j] = i == j ? 1.f : 0.f;
  for (int sweep = 0; sweep < 8; sweep++) {
    const float off = A[0][1] * A[0][1] + A[0][2] * A[0][2] + A[1][2] * A[1][2];
    if (off <=
        1e-14f * (A[0][0] * A[0][0] + A[1][1] * A[1][1] + A[2][2] * A[2][2]))
      break; // converged to float precision
    for (int p = 0; p < 2; p++) {
      for (int q = p + 1; q < 3; q++) {
        if (A[p][q] == 0)
          continue;
        const float theta = (A[q][q] - A[p][p]) / (2 * A[p][q]);
        const float t =
            (theta >= 0 ? 1 : -1) / (fabsf(theta) + sqrtf(theta * theta + 1));
        const float c = 1 / sqrtf(t * t + 1), s = t * c;
        for (int k = 0; k < 3; k++) { // A ← Jᵀ A J
          const float akp = A[k][p], akq = A[k][q];
          A[k][p] = c * akp - s * akq;
          A[k][q] = s * akp + c * akq;
        }
        for (int k = 0; k < 3; k++) {
          const float apk = A[p][k], aqk = A[q][k];
          A[p][k] = c * apk - s * aqk;
          A[q][k] = s * apk + c * aqk;
        }
        for (int k = 0; k < 3; k++) {
          const float vkp = V[k][p], vkq = V[k][q];
          V[k][p] = c * vkp - s * vkq;
          V[k][q] = s * vkp + c * vkq;
        }
      }
    }
  }
}

// The rotation R maximising tr(Rᵀ M): R = U Vᵀ from M's SVD, made proper.
// M = Σ q pᵀ over tag-plane points (p.z = 0), so it has rank 2 and only R's
// first two columns are determined by M; the third is their cross product.
static inline void nearest_rotation(const float M[3][3], float R[3][3]) {
  float A[3][3], V[3][3];
  for (int i = 0; i < 3; i++) // A = Mᵀ M
    for (int j = 0; j < 3; j++)
      A[i][j] = M[0][i] * M[0][j] + M[1][i] * M[1][j] + M[2][i] * M[2][j];
  sym3_eigen(A, V);
  // the two largest eigenvalues' vectors (the third is ~0: rank 2)
  const int small = A[0][0] <= A[1][1] && A[0][0] <= A[2][2] ? 0
                    : A[1][1] <= A[2][2]                     ? 1
                                                             : 2;
  const int idx[2] = {small == 0 ? 1 : 0, small == 2 ? 1 : 2};
  float u[2][3], v[2][3];
  for (int n = 0; n < 2; n++) { // u = M v / |M v|
    for (int i = 0; i < 3; i++)
      v[n][i] = V[i][idx[n]];
    float norm = 0;
    for (int i = 0; i < 3; i++) {
      u[n][i] = M[i][0] * v[n][0] + M[i][1] * v[n][1] + M[i][2] * v[n][2];
      norm += u[n][i] * u[n][i];
    }
    norm = sqrtf(norm);
    for (int i = 0; i < 3; i++)
      u[n][i] /= norm;
  }
  for (int i = 0; i < 3; i++)
    for (int j = 0; j < 2; j++)
      R[i][j] = u[0][i] * v[0][j] + u[1][i] * v[1][j];
  R[0][2] = R[1][0] * R[2][1] - R[2][0] * R[1][1];
  R[1][2] = R[2][0] * R[0][1] - R[0][0] * R[2][1];
  R[2][2] = R[0][0] * R[1][1] - R[1][0] * R[0][1];
}

// v: normalised image rays of the corners, p: the corners in the tag frame
// (z = 0). R: initial guess in, refined rotation out; t: translation out.
// Returns the object-space error after the last step, like apriltag's.
static inline float orthogonal_iteration4(const float v[4][3],
                                          const float p[4][3], float R[3][3],
                                          float t[3], int steps) {
  float F[4][3][3], pm[3] = {0, 0, 0}, pr[4][3], avgF[3][3] = {};
  for (int j = 0; j < 4; j++) {
    const float vv = v[j][0] * v[j][0] + v[j][1] * v[j][1] + v[j][2] * v[j][2];
    for (int a = 0; a < 3; a++) {
      pm[a] += p[j][a] / 4;
      for (int b = 0; b < 3; b++) {
        F[j][a][b] = v[j][a] * v[j][b] / vv; // projection onto the ray
        avgF[a][b] += F[j][a][b] / 4;
      }
    }
  }
  for (int j = 0; j < 4; j++)
    for (int a = 0; a < 3; a++)
      pr[j][a] = p[j][a] - pm[a];

  // M1inv = (I - mean F)⁻¹, by the adjugate
  float M1[3][3], M1inv[3][3];
  for (int a = 0; a < 3; a++)
    for (int b = 0; b < 3; b++)
      M1[a][b] = (a == b ? 1.f : 0.f) - avgF[a][b];
  for (int a = 0; a < 3; a++)
    for (int b = 0; b < 3; b++) {
      const int a1 = (b + 1) % 3, a2 = (b + 2) % 3, b1 = (a + 1) % 3,
                b2 = (a + 2) % 3;
      M1inv[a][b] = M1[a1][b1] * M1[a2][b2] - M1[a1][b2] * M1[a2][b1];
    }
  const float det =
      M1[0][0] * M1inv[0][0] + M1[0][1] * M1inv[1][0] + M1[0][2] * M1inv[2][0];
  for (int a = 0; a < 3; a++)
    for (int b = 0; b < 3; b++)
      M1inv[a][b] /= det;

  float err = HUGE_VALF;
  for (int step = 0; step < steps; step++) {
    float Rp[4][3];
    for (int j = 0; j < 4; j++)
      for (int a = 0; a < 3; a++)
        Rp[j][a] = R[a][0] * p[j][0] + R[a][1] * p[j][1] + R[a][2] * p[j][2];

    // t = M1inv · mean_j (F_j - I) R p_j
    float M2[3] = {0, 0, 0};
    for (int j = 0; j < 4; j++)
      for (int a = 0; a < 3; a++)
        M2[a] += (F[j][a][0] * Rp[j][0] + F[j][a][1] * Rp[j][1] +
                  F[j][a][2] * Rp[j][2] - Rp[j][a]) /
                 4;
    for (int a = 0; a < 3; a++)
      t[a] = M1inv[a][0] * M2[0] + M1inv[a][1] * M2[1] + M1inv[a][2] * M2[2];

    // q_j = F_j (R p_j + t); R = nearest rotation to Σ (q_j - q̄) p̃_jᵀ
    float q[4][3], qm[3] = {0, 0, 0};
    for (int j = 0; j < 4; j++)
      for (int a = 0; a < 3; a++) {
        q[j][a] = F[j][a][0] * (Rp[j][0] + t[0]) +
                  F[j][a][1] * (Rp[j][1] + t[1]) +
                  F[j][a][2] * (Rp[j][2] + t[2]);
        qm[a] += q[j][a] / 4;
      }
    float M3[3][3] = {};
    for (int j = 0; j < 4; j++)
      for (int a = 0; a < 3; a++)
        for (int b = 0; b < 3; b++)
          M3[a][b] += (q[j][a] - qm[a]) * pr[j][b];
    nearest_rotation(M3, R);

    // object-space error, with the new R and the t from this step
    err = 0;
    for (int j = 0; j < 4; j++) {
      float x[3];
      for (int a = 0; a < 3; a++)
        x[a] = R[a][0] * p[j][0] + R[a][1] * p[j][1] + R[a][2] * p[j][2] + t[a];
      for (int a = 0; a < 3; a++) {
        const float e =
            x[a] - (F[j][a][0] * x[0] + F[j][a][1] * x[1] + F[j][a][2] * x[2]);
        err += e * e;
      }
    }
  }
  return err;
}

// Declared only in apriltag_pose.c
extern "C" matd_t *fix_pose_ambiguities(matd_t **v, matd_t **p, matd_t *t,
                                        matd_t *R, int n_points);

// Orthogonal-iteration steps per starting pose. apriltag's estimate_tag_pose()
// uses 50, which leaves the tail unconverged: over 2000 simulated tags with
// 0.1 px corner noise, 5 % end ≥ 2.3° from the converged pose (p90 error vs
// truth 1.58°, p99 13.2°). 200 steps: p90 1.31°, p99 6.0°, within 0.13° of
// converged at p95; the median is unchanged, and 500 adds nothing.
static constexpr int POSE_STEPS = 200;

// Pose of a detection made on an image `div` times smaller than the frame
// `cam` describes. Undistorts det's corners in place. apriltag's
// estimate_tag_pose() — iterate from the homography pose, iterate from the
// other local minimum, keep the better — with orthogonal_iteration4() for its
// iteration: the same answer to float precision, ~25× faster on the board.
static inline TagPose tag_pose(apriltag_detection_t *det,
                               const CameraModel &cam, int div, double tag_size,
                               int steps = POSE_STEPS) {
  if (has_distortion(cam))
    undistort_detection(det, cam, div);
  apriltag_detection_info_t info = {det,          tag_size,     cam.fx / div,
                                    cam.fy / div, cam.cx / div, cam.cy / div};
  apriltag_pose_t pose; // homography-based first guess
  estimate_pose_for_tag_homography(&info, &pose);

  const float s = tag_size / 2; // corners in apriltag's order
  float p[4][3] = {{-s, s, 0}, {s, s, 0}, {s, -s, 0}, {-s, -s, 0}};
  float v[4][3], R[3][3], t[3];
  for (int i = 0; i < 4; i++) {
    v[i][0] = (det->p[i][0] - info.cx) / info.fx;
    v[i][1] = (det->p[i][1] - info.cy) / info.fy;
    v[i][2] = 1;
  }
  for (int i = 0; i < 3; i++)
    for (int j = 0; j < 3; j++)
      R[i][j] = MATD_EL(pose.R, i, j);
  float err = orthogonal_iteration4(v, p, R, t, steps);

  // The other local minimum of a planar target's pose
  for (int i = 0; i < 3; i++) {
    MATD_EL(pose.t, i, 0) = t[i];
    for (int j = 0; j < 3; j++)
      MATD_EL(pose.R, i, j) = R[i][j];
  }
  matd_t *pm[4], *vm[4];
  for (int i = 0; i < 4; i++) {
    pm[i] = matd_create(3, 1);
    vm[i] = matd_create(3, 1);
    for (int j = 0; j < 3; j++) {
      MATD_EL(pm[i], j, 0) = p[i][j];
      MATD_EL(vm[i], j, 0) = v[i][j];
    }
  }
  matd_t *R2m = fix_pose_ambiguities(vm, pm, pose.t, pose.R, 4);
  if (R2m) {
    float R2[3][3], t2[3];
    for (int i = 0; i < 3; i++)
      for (int j = 0; j < 3; j++)
        R2[i][j] = MATD_EL(R2m, i, j);
    const float err2 = orthogonal_iteration4(v, p, R2, t2, steps);
    if (err2 < err) {
      err = err2;
      memcpy(R, R2, sizeof(R));
      memcpy(t, t2, sizeof(t));
    }
    matd_destroy(R2m);
  }
  for (int i = 0; i < 4; i++) {
    matd_destroy(pm[i]);
    matd_destroy(vm[i]);
  }

  TagPose out;
  out.err = err;
  for (int i = 0; i < 3; i++) {
    out.t[i] = t[i];
    for (int j = 0; j < 3; j++)
      MATD_EL(pose.R, i, j) = R[i][j];
  }
  mat_to_quat(pose.R, out.q);
  matd_destroy(pose.R);
  matd_destroy(pose.t);
  return out;
}
