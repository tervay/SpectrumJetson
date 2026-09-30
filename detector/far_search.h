// SpectrumJetson: far-tag search. The 971 GPU detector finds quads on a half-size image (bos hard-
// codes quad_decimate 2), so tags under ~20 px are missed; on the full-size image it finds them down
// to ~12 px (~1.7x the range; docs/TECHNICAL.md "Far-tag search"). Searching every frame at full
// size costs ~2.75x the GPU, so this only does it when the robot is short of a good pose:
//
//  - A camera has a *good view* when it sees at least kGoodTags tags of at least kGoodSidePx with a
//    decision margin of at least kGoodMargin (near tags: a solid multi-tag pose).
//  - When no camera has had a good view for kStarveMs, the cameras are *starved*: one camera at a
//    time (round robin, at most sweeps_per_s across all of them) runs a full-size search of its
//    frame (a 2x nearest-neighbour upscale through a shared 2560x1600 detector; the half-size
//    search then sees every pixel). Tags it finds that the normal search didn't are tracked with
//    full-size crops (160x160 upscaled to 320x320, one detector per camera) on that camera's
//    following frames.
//  - As soon as any camera has a good view again, all of it stops: cameras cost what they did.
//
// Everything is in image coordinates of the camera's frame: corners, centre and homography are
// scaled back from the upscaled (and cropped) images. Shared by lib971apriltag.so (live, through
// GpuDetectorJNI.cc) and fieldcal_detect (replay), so both run the same policy.
#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include <opencv2/core/mat.hpp>

#include "third_party/971apriltag/apriltag.h"

struct apriltag_family;
typedef struct apriltag_family apriltag_family_t;

namespace far_search {

// One detection, copied out of the detector (its own list is reused on the next Detect).
struct Det {
  const apriltag_family_t *family = nullptr;
  int id = 0;
  int hamming = 0;
  float margin = 0;
  std::array<double, 9> H{};  // row-major homography, tag coordinates -> image
  std::array<double, 2> c{};
  std::array<double, 8> p{};  // corners x0 y0 x1 y1 ...
  bool far = false;           // found by the far search, not the normal one
  double side() const;        // mean side length in pixels
};

struct Config {
  bool enabled = true;
  double sweeps_per_s = 30;  // full-size searches a second, across all cameras
};

void SetConfig(const Config &c);
Config GetConfig();

// Counters since start, for the stats line and /api/farSearch.
struct Counters {
  long sweeps = 0;       // full-size searches run
  long crops = 0;        // crop searches run
  long far_tags = 0;     // extra detections returned (tags the normal search missed)
  long starved_ms = 0;   // time spent starved
  bool starved = false;  // right now
  double sweep_ms = 0;   // total time in full-size searches
  double crop_ms = 0;    // total time in crop searches
};
Counters GetCounters();

// Called for each camera frame after its normal detection, with the camera's detector settings
// (their family and thresholds are copied into the far-search detectors), calibration, and the
// normal detections. Returns the extra detections to add (possibly none). `camera` is the
// caller's detector handle (0..kMaxCameras-1). Thread-safe across cameras; one call per camera
// at a time. `now_us` is the frame's time in microseconds (steady clock live; recording time in
// replay).
constexpr int kMaxCameras = 10;
std::vector<Det> Process(int camera, const cv::Mat &gray, const std::vector<Det> &normal,
                         const apriltag_detector_t *td, frc::apriltag::CameraMatrix camera_matrix,
                         frc::apriltag::DistCoeffs dist_coeffs, int64_t now_us);

// Copies the detector's current detections.
std::vector<Det> Copy(const zarray_t *detections);

// Forget a camera (its detector was destroyed).
void Forget(int camera);

// The good-view rule, exposed for tests.
constexpr int kGoodTags = 2;
constexpr double kGoodSidePx = 40;
constexpr float kGoodMargin = 30;
constexpr int64_t kStarveUs = 250'000;
bool GoodView(const std::vector<Det> &dets);

}  // namespace far_search
