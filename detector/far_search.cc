// SpectrumJetson: far-tag search (see far_search.h).
#include "far_search.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>

#include <cstring>

#include "absl/status/status.h"
#include "apriltag/apriltag.h"
#include "apriltag/tag36h11.h"
#include "third_party/971apriltag/apriltag.h"

namespace far_search {
namespace {

// The detector's coordinates in a 2x nearest-neighbour upscale U, and in the frame X:
// X = U / 2 + kHalfPixel. Measured (far_search_test, tags both searches find): with 0.25, as the
// AprilTag library's decimation convention suggests, the full-size corners came out +0.24 px off
// in x and y; the 971 detector puts pixel edges at whole numbers, so it's 0.
constexpr double kHalfPixel = 0;
// A tracked tag this big (or bigger) that the normal search finds is back in its range: stop
// tracking it. Smaller ones sit on the edge of the normal search, found some frames and not
// others, so they're still tracked (with the normal search's position when it has one).
constexpr double kNormalRangePx = 24;
constexpr int kCropSize = 160;           // frame pixels around a tracked tag
constexpr int kMaxTracksPerCamera = 3;
// At most one crop search per camera frame (~2.3 ms each, mostly the detector's fixed cost),
// taking the tracked tags in turn; the others keep their last position until their turn.
constexpr int kMaxCropsPerFrame = 1;
constexpr int64_t kTrackTimeoutUs = 300'000;  // drop a track not seen for this long
constexpr int64_t kActiveUs = 1'000'000;      // a camera counts for the round robin for this long
constexpr size_t kMaxDetectorPixels = 1u << 22;  // the 971 detector's limit

struct Track {
  int id = 0;
  double cx = 0, cy = 0;
  int64_t last_seen_us = 0;
};

// A detector with its own copy of the camera's detector settings.
struct FarDetector {
  frc::apriltag::GpuDetector *gpu = nullptr;
  apriltag_detector_t *td = nullptr;
  // Its own family: adding one to a detector puts that detector's decode table in the family, so
  // sharing the camera's would overwrite (and on destroy free) the camera detector's table.
  apriltag_family_t *family = nullptr;
  size_t width = 0, height = 0;
  float mwbd = -1, line_mse = -1;
  // The upscaled image handed to the detector (kept: it reads it during Detect). Plain memory,
  // not cv::Mat: this runs inside PhotonVision's JVM, which has its own OpenCV, so the library
  // uses only OpenCV's header-only parts (a second OpenCV's functions could clash with it).
  std::vector<uint8_t> up;

  ~FarDetector() { Reset(); }
  void Reset() {
    delete gpu;
    gpu = nullptr;
    if (td) {
      apriltag_detector_destroy(td);  // also destroys its worker pool
      td = nullptr;
    }
    if (family) {
      tag36h11_destroy(family);
      family = nullptr;
    }
  }
  // (Re)builds for this size and these settings. Returns false if it can't.
  bool Ensure(size_t w, size_t h, const apriltag_detector_t *src, frc::apriltag::CameraMatrix cm,
              frc::apriltag::DistCoeffs dc) {
    if (gpu && width == w && height == h && mwbd == src->qtp.min_white_black_diff &&
        line_mse == src->qtp.max_line_fit_mse)
      return true;
    Reset();
    if (w * h > kMaxDetectorPixels || w % 8 || h % 8) return false;
    apriltag_family_t *src_family = nullptr;
    if (!src->tag_families || zarray_size(src->tag_families) < 1) return false;
    zarray_get(src->tag_families, 0, &src_family);
    if (std::string(src_family->name) != "tag36h11") return false;  // FRC's family; the only one here
    family = tag36h11_create();
    td = apriltag_detector_create();
    apriltag_detector_add_family_bits(td, family, 1);
    td->qtp = src->qtp;
    td->decode_sharpening = src->decode_sharpening;
    td->refine_edges = src->refine_edges;
    td->nthreads = std::max(1, src->nthreads);
    td->wp = workerpool_create(td->nthreads);
    td->debug = false;
    try {
      gpu = new frc::apriltag::GpuDetector(w, h, td, cm, dc, vision::ImageFormat::MONO8);
    } catch (const std::exception &e) {
      std::cout << "971 far search: detector " << w << "x" << h << " failed: " << e.what()
                << std::endl;
      Reset();
      return false;
    }
    width = w;
    height = h;
    mwbd = src->qtp.min_white_black_diff;
    line_mse = src->qtp.max_line_fit_mse;
    up.assign(w * h, 0);
    return true;
  }
};

struct Camera {
  std::vector<Track> tracks;
  std::unique_ptr<FarDetector> crop;
  int64_t last_frame_us = 0;
  int64_t last_sweep_us = 0;
};

std::mutex state_mu;
Config config;
Counters counters;
Camera cameras[kMaxCameras];
int64_t last_good_us = -1;       // any camera's last good view
int64_t last_any_sweep_us = -1;  // for the sweeps-a-second budget
int64_t last_starve_check_us = -1;
// Time guard: after a full-size search that took T, the next waits at least 5 T, so the far search
// can't take more than ~20% of the time even in a scene that makes the searches slow. (Normally
// the budget limits it first: 30 a second at ~5.5 ms each is ~16%.)
int64_t next_sweep_allowed_us = -1;
constexpr double kMaxSweepShare = 0.20;

std::mutex full_mu;  // the shared full-size detector, one camera at a time
FarDetector full;

// Copies the w x h region at (x0, y0) of an 8-bit image into dst at twice the size, each pixel
// repeated 2x2 (a nearest-neighbour upscale).
void Upscale2x(const uint8_t *src, size_t stride, int x0, int y0, int w, int h, uint8_t *dst) {
  const size_t dw = 2 * static_cast<size_t>(w);
  for (int y = 0; y < h; ++y) {
    const uint8_t *s = src + static_cast<size_t>(y0 + y) * stride + x0;
    uint8_t *d = dst + 2 * static_cast<size_t>(y) * dw;
    for (int x = 0; x < w; ++x) d[2 * x] = d[2 * x + 1] = s[x];
    std::memcpy(d + dw, d, dw);
  }
}

// Maps a detection from an upscaled (and cropped at x0, y0) image back to the frame.
Det Unscale(const Det &d, double x0, double y0) {
  Det o = d;
  o.far = true;
  for (int i = 0; i < 4; ++i) {
    o.p[2 * i] = d.p[2 * i] / 2 + kHalfPixel + x0;
    o.p[2 * i + 1] = d.p[2 * i + 1] / 2 + kHalfPixel + y0;
  }
  o.c[0] = d.c[0] / 2 + kHalfPixel + x0;
  o.c[1] = d.c[1] / 2 + kHalfPixel + y0;
  // H' = T H with T = [[1/2, 0, kHalfPixel + x0], [0, 1/2, kHalfPixel + y0], [0, 0, 1]].
  const auto &H = d.H;
  const double tx = kHalfPixel + x0, ty = kHalfPixel + y0;
  for (int col = 0; col < 3; ++col) {
    o.H[0 * 3 + col] = H[0 * 3 + col] / 2 + tx * H[2 * 3 + col];
    o.H[1 * 3 + col] = H[1 * 3 + col] / 2 + ty * H[2 * 3 + col];
    o.H[2 * 3 + col] = H[2 * 3 + col];
  }
  return o;
}

// The camera matrix for an image upscaled 2x after cropping at x0, y0 (inverse of Unscale).
frc::apriltag::CameraMatrix Scaled(frc::apriltag::CameraMatrix cm, double x0, double y0) {
  cm.fx *= 2;
  cm.fy *= 2;
  cm.cx = 2 * (cm.cx - kHalfPixel - x0);
  cm.cy = 2 * (cm.cy - kHalfPixel - y0);
  return cm;
}

bool Has(const std::vector<Det> &dets, int id) {
  return std::any_of(dets.begin(), dets.end(), [&](const Det &d) { return d.id == id; });
}

}  // namespace

double Det::side() const {
  double s = 0;
  for (int i = 0; i < 4; ++i) {
    const int j = (i + 1) % 4;
    s += std::hypot(p[2 * j] - p[2 * i], p[2 * j + 1] - p[2 * i + 1]);
  }
  return s / 4;
}

std::vector<Det> Copy(const zarray_t *detections) {
  std::vector<Det> out;
  const int n = detections ? zarray_size(detections) : 0;
  out.reserve(n);
  for (int i = 0; i < n; ++i) {
    apriltag_detection_t *det;
    zarray_get(detections, i, &det);
    Det d;
    d.family = det->family;
    d.id = det->id;
    d.hamming = det->hamming;
    d.margin = det->decision_margin;
    for (int k = 0; k < 9 && det->H && k < det->H->nrows * det->H->ncols; ++k) d.H[k] = det->H->data[k];
    d.c = {det->c[0], det->c[1]};
    for (int k = 0; k < 4; ++k) {
      d.p[2 * k] = det->p[k][0];
      d.p[2 * k + 1] = det->p[k][1];
    }
    out.push_back(d);
  }
  return out;
}

bool GoodView(const std::vector<Det> &dets) {
  int n = 0;
  for (const auto &d : dets) {
    if (!d.far && d.side() >= kGoodSidePx && d.margin >= kGoodMargin) ++n;
  }
  return n >= kGoodTags;
}

void SetConfig(const Config &c) {
  std::lock_guard<std::mutex> lock(state_mu);
  if (c.enabled != config.enabled || c.sweeps_per_s != config.sweeps_per_s) {
    std::cout << "971 far search: " << (c.enabled ? "on" : "off");
    if (c.enabled) std::cout << ", up to " << c.sweeps_per_s << " full-size searches a second";
    std::cout << std::endl;
  }
  config = c;
}

Config GetConfig() {
  std::lock_guard<std::mutex> lock(state_mu);
  return config;
}

Counters GetCounters() {
  std::lock_guard<std::mutex> lock(state_mu);
  return counters;
}

void Forget(int camera) {
  if (camera < 0 || camera >= kMaxCameras) return;
  std::lock_guard<std::mutex> lock(state_mu);
  cameras[camera].tracks.clear();
  cameras[camera].crop.reset();
  cameras[camera].last_frame_us = 0;
  cameras[camera].last_sweep_us = 0;
}

std::vector<Det> Process(int camera, const cv::Mat &gray, const std::vector<Det> &normal,
                         const apriltag_detector_t *td, frc::apriltag::CameraMatrix cm,
                         frc::apriltag::DistCoeffs dc, int64_t now_us) {
  std::vector<Det> extras;
  if (camera < 0 || camera >= kMaxCameras || gray.type() != CV_8UC1 || gray.cols < kCropSize ||
      gray.rows < kCropSize)
    return extras;
  Camera &cam = cameras[camera];
  // Build the shared full-size detector on the first frame (PhotonVision starting), not on the
  // first search: building it takes ~200 ms, which would stall a camera just as it lost its view.
  if (config.enabled) {
    std::unique_lock<std::mutex> lock(full_mu, std::try_to_lock);
    if (lock.owns_lock() && !full.gpu) full.Ensure(2 * gray.cols, 2 * gray.rows, td, Scaled(cm, 0, 0), dc);
  }
  bool starved, sweep;
  {
    std::lock_guard<std::mutex> lock(state_mu);
    cam.last_frame_us = now_us;
    if (last_good_us < 0) last_good_us = now_us;  // start as if just good: no sweep in the first 250 ms
    if (GoodView(normal)) last_good_us = now_us;
    starved = config.enabled && now_us - last_good_us > kStarveUs;
    if (last_starve_check_us >= 0 && counters.starved && now_us > last_starve_check_us) {
      counters.starved_ms += (now_us - last_starve_check_us) / 1000;
    }
    last_starve_check_us = now_us;
    counters.starved = starved;
    if (!starved) {
      cam.tracks.clear();
      return extras;
    }
    // Round robin: this camera's turn if its last sweep is the oldest among active cameras, and
    // the budget allows one now.
    sweep = config.sweeps_per_s > 0 &&
            (last_any_sweep_us < 0 || now_us - last_any_sweep_us >= 1e6 / config.sweeps_per_s) &&
            now_us >= next_sweep_allowed_us;
    for (int i = 0; sweep && i < kMaxCameras; ++i) {
      const Camera &o = cameras[i];
      if (i == camera || now_us - o.last_frame_us > kActiveUs || o.last_frame_us == 0) continue;
      if (o.last_sweep_us < cam.last_sweep_us || (o.last_sweep_us == cam.last_sweep_us && i < camera)) {
        sweep = false;
      }
    }
    if (sweep) {
      last_any_sweep_us = now_us;
      cam.last_sweep_us = now_us;
    }
  }

  const int W = gray.cols, H = gray.rows;
  using clk = std::chrono::steady_clock;
  long crops_run = 0;
  double crop_ms = 0, sweep_ms = 0;

  // Crops around tracked far tags that the normal search still doesn't see.
  std::vector<Track> kept;
  int crops_left = kMaxCropsPerFrame;
  // Longest-unseen first, so the tracked tags take turns.
  std::sort(cam.tracks.begin(), cam.tracks.end(),
            [](const Track &a, const Track &b) { return a.last_seen_us < b.last_seen_us; });
  for (auto &t : cam.tracks) {
    auto n = std::find_if(normal.begin(), normal.end(), [&](const Det &d) { return d.id == t.id; });
    if (n != normal.end()) {
      if (n->side() >= kNormalRangePx) continue;  // comfortably in the normal range: done
      t.cx = n->c[0];  // the normal search has it this frame: no crop, keep following
      t.cy = n->c[1];
      t.last_seen_us = now_us;
      kept.push_back(t);
      continue;
    }
    if (now_us - t.last_seen_us > kTrackTimeoutUs) continue;
    if (crops_left-- <= 0) {
      kept.push_back(t);  // its turn comes on a later frame
      continue;
    }
    const int x0 = std::clamp(static_cast<int>(std::lround(t.cx)) - kCropSize / 2, 0, W - kCropSize);
    const int y0 = std::clamp(static_cast<int>(std::lround(t.cy)) - kCropSize / 2, 0, H - kCropSize);
    if (!cam.crop) cam.crop = std::make_unique<FarDetector>();
    auto t0 = clk::now();
    if (cam.crop->Ensure(2 * kCropSize, 2 * kCropSize, td, Scaled(cm, x0, y0), dc)) {
      Upscale2x(gray.ptr<uint8_t>(), gray.step, x0, y0, kCropSize, kCropSize, cam.crop->up.data());
      cam.crop->gpu->SetCameraMatrix(Scaled(cm, x0, y0));
      cam.crop->gpu->SetDistortionCoefficients(dc);
      if (cam.crop->gpu->Detect(cam.crop->up.data(), nullptr).ok()) {
        for (const auto &d : Copy(cam.crop->gpu->Detections())) {
          if (d.id != t.id) continue;
          Det o = Unscale(d, x0, y0);
          t.cx = o.c[0];
          t.cy = o.c[1];
          t.last_seen_us = now_us;
          extras.push_back(o);
          break;
        }
      }
      ++crops_run;
    }
    crop_ms += std::chrono::duration<double, std::milli>(clk::now() - t0).count();
    kept.push_back(t);
  }
  cam.tracks = kept;

  // A full-size search of the whole frame, on this camera's turn.
  if (sweep) {
    std::lock_guard<std::mutex> lock(full_mu);
    auto t0 = clk::now();
    if (full.Ensure(2 * W, 2 * H, td, Scaled(cm, 0, 0), dc)) {
      Upscale2x(gray.ptr<uint8_t>(), gray.step, 0, 0, W, H, full.up.data());
      full.gpu->SetCameraMatrix(Scaled(cm, 0, 0));
      full.gpu->SetDistortionCoefficients(dc);
      if (full.gpu->Detect(full.up.data(), nullptr).ok()) {
        for (const auto &d : Copy(full.gpu->Detections())) {
          if (Has(normal, d.id) || Has(extras, d.id)) continue;
          Det o = Unscale(d, 0, 0);
          extras.push_back(o);
          auto it = std::find_if(cam.tracks.begin(), cam.tracks.end(), [&](const Track &t) { return t.id == o.id; });
          if (it != cam.tracks.end()) {
            it->cx = o.c[0];
            it->cy = o.c[1];
            it->last_seen_us = now_us;
          } else if (static_cast<int>(cam.tracks.size()) < kMaxTracksPerCamera) {
            cam.tracks.push_back({o.id, o.c[0], o.c[1], now_us});
          }
        }
      }
    }
    sweep_ms = std::chrono::duration<double, std::milli>(clk::now() - t0).count();
  }

  std::lock_guard<std::mutex> lock(state_mu);
  if (sweep) next_sweep_allowed_us = now_us + static_cast<int64_t>(sweep_ms * 1000 / kMaxSweepShare);
  counters.sweeps += sweep;
  counters.crops += crops_run;
  counters.far_tags += static_cast<long>(extras.size());
  counters.sweep_ms += sweep_ms;
  counters.crop_ms += crop_ms;
  return extras;
}

}  // namespace far_search
