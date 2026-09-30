// SpectrumJetson: bench test for the far-tag search (far_search.h), on the Jetson, no camera
// needed. Renders real tag36h11 tags at chosen sizes into 1280x800 frames (slightly blurred, with
// noise, like a camera), and checks:
//   1. range: the smallest tag the normal (half-size) search and the full-size search each find;
//   2. corners: where both find a tag, the full-size search's corners match the normal one's;
//   3. policy: nothing extra while a camera has a good view; searches start after 250 ms without
//      one, find the far tag, and crops then track it as it moves; all of it stops as soon as a
//      good view returns;
//   4. budget: full-size searches at most sweeps_per_s, shared round robin between two cameras;
//   5. off: with the search off, nothing extra.
// Exit status 0 if every check passes. Run: build/far_search_test (tests/far-search/run.sh).
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <iostream>
#include <random>
#include <string>
#include <vector>

#include <cuda_runtime.h>
#include <jpeglib.h>

#include <filesystem>
#include <fstream>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>

#include "absl/status/status.h"
#include "apriltag/apriltag.h"
#include "apriltag/tag36h11.h"
#include "far_search.h"
#include "third_party/971apriltag/apriltag.h"

namespace {

constexpr int kW = 1280, kH = 800;
int failures = 0;

void Check(bool ok, const std::string &what) {
  std::printf("  %s  %s\n", ok ? "PASS" : "FAIL", what.c_str());
  if (!ok) ++failures;
}

apriltag_family_t *family = nullptr;

// Draws tag `id` with its black square `side` px across, centred at (cx, cy), rotated `deg`.
void DrawTag(cv::Mat &img, int id, double side, double cx, double cy, double deg = 7) {
  image_u8_t *t = apriltag_to_image(family, id);  // total_width cells, including the white border
  cv::Mat tag(t->height, t->width, CV_8UC1, t->buf, t->stride);
  const double cell = side / family->width_at_border;
  const int big = 40;  // render large, then warp down with interpolation (a camera's blur)
  cv::Mat large;
  cv::resize(tag, large, cv::Size(tag.cols * big, tag.rows * big), 0, 0, cv::INTER_NEAREST);
  const double s = cell / big;
  const double a = deg * M_PI / 180;
  // Maps the large tag's centre to (cx, cy), scaled by s and rotated by a.
  const double lc = large.cols / 2.0;
  cv::Mat M = (cv::Mat_<double>(2, 3) << s * std::cos(a), -s * std::sin(a), 0, s * std::sin(a),
               s * std::cos(a), 0);
  M.at<double>(0, 2) = cx - (M.at<double>(0, 0) * lc + M.at<double>(0, 1) * lc);
  M.at<double>(1, 2) = cy - (M.at<double>(1, 0) * lc + M.at<double>(1, 1) * lc);
  cv::Mat warped(img.size(), CV_8UC1, cv::Scalar(0));
  cv::Mat mask(img.size(), CV_8UC1, cv::Scalar(0));
  cv::warpAffine(large, warped, M, img.size(), cv::INTER_AREA, cv::BORDER_CONSTANT, cv::Scalar(0));
  cv::warpAffine(cv::Mat(large.size(), CV_8UC1, cv::Scalar(255)), mask, M, img.size(), cv::INTER_AREA,
                 cv::BORDER_CONSTANT, cv::Scalar(0));
  // Blend by coverage, so the tag's edge pixels are partly background, like a real one.
  for (int y = 0; y < img.rows; ++y) {
    for (int x = 0; x < img.cols; ++x) {
      const int m = mask.at<uint8_t>(y, x);
      if (!m) continue;
      const int v = warped.at<uint8_t>(y, x) * 255 / std::max(1, m);  // tag value where covered
      const int tagv = v > 127 ? 210 : 30;                          // white 210, black 30
      img.at<uint8_t>(y, x) = static_cast<uint8_t>((tagv * m + img.at<uint8_t>(y, x) * (255 - m)) / 255);
    }
  }
  image_u8_destroy(t);
}

cv::Mat Frame(std::mt19937 &rng) {
  cv::Mat img(kH, kW, CV_8UC1, cv::Scalar(110));
  return img;
}

// A camera's softness and noise: blur, sensor noise, then smoothed again (as its JPEG does; raw
// per-pixel noise is much harsher than a real camera's and makes the full-size search slow).
void Finish(cv::Mat &img, std::mt19937 &rng) {
  cv::GaussianBlur(img, img, cv::Size(3, 3), 0.6);
  cv::Mat n(img.size(), CV_32F);
  cv::randn(n, 0, 1.5);
  cv::GaussianBlur(n, n, cv::Size(3, 3), 0.8);
  cv::Mat f;
  img.convertTo(f, CV_32F);
  f += n;
  f.convertTo(img, CV_8U);
}

apriltag_detector_t *MakeTd(apriltag_family_t *fam) {
  apriltag_detector_t *td = apriltag_detector_create();
  apriltag_detector_add_family_bits(td, fam, 1);
  td->nthreads = 4;
  td->wp = workerpool_create(td->nthreads);
  td->qtp.min_white_black_diff = 5;  // the live default (SPECTRUM_971_MIN_WHITE_BLACK_DIFF)
  td->qtp.max_line_fit_mse = 10;
  td->debug = false;
  return td;
}

const frc::apriltag::CameraMatrix kCm{1, 1, 1, 1};  // as the live wrapper before a calibration
frc::apriltag::DistCoeffs Dc() {
  frc::apriltag::DistCoeffs d{};
  d.num_params = 5;
  return d;
}

std::vector<far_search::Det> DetectNormal(frc::apriltag::GpuDetector &gpu, const cv::Mat &img) {
  if (!gpu.Detect(img.ptr<uint8_t>(), nullptr).ok()) return {};
  auto dets = far_search::Copy(gpu.Detections());
  // As PhotonVision keeps them: decision margin 15 and up.
  std::vector<far_search::Det> kept;
  for (auto &d : dets)
    if (d.margin >= 15) kept.push_back(d);
  return kept;
}

bool Found(const std::vector<far_search::Det> &d, int id) {
  for (auto &x : d)
    if (x.id == id) return true;
  return false;
}

// Grayscale JPEG, quality 85 (about what the cameras send).
std::vector<unsigned char> EncodeJpeg(const cv::Mat &img) {
  jpeg_compress_struct c;
  jpeg_error_mgr err;
  c.err = jpeg_std_error(&err);
  jpeg_create_compress(&c);
  unsigned char *buf = nullptr;
  unsigned long size = 0;
  jpeg_mem_dest(&c, &buf, &size);
  c.image_width = img.cols;
  c.image_height = img.rows;
  c.input_components = 1;
  c.in_color_space = JCS_GRAYSCALE;
  jpeg_set_defaults(&c);
  jpeg_set_quality(&c, 85, TRUE);
  jpeg_start_compress(&c, TRUE);
  while (c.next_scanline < c.image_height) {
    JSAMPROW row = const_cast<uint8_t *>(img.ptr<uint8_t>(c.next_scanline));
    jpeg_write_scanlines(&c, &row, 1);
  }
  jpeg_finish_compress(&c);
  std::vector<unsigned char> out(buf, buf + size);
  free(buf);
  jpeg_destroy_compress(&c);
  return out;
}

// --write-session DIR: a synthetic Rewind session for far_replay, with a known answer. Two
// cameras, 3 s at 60 fps. CamA: near tags 1 and 2 (70 px) for the first and last second, and a
// far tag 7 (14 px, moving) all along; CamB: no tags. Expected from far_replay: the far search
// finds tag 7 on CamA frames the normal search misses, only in the middle second (after the
// 250 ms wait); with --off, nothing extra.
int WriteSession(const std::string &dir) {
  namespace fs = std::filesystem;
  std::mt19937 rng(7);
  for (const char *cam : {"CamA", "CamB"}) {
    fs::create_directories(fs::path(dir) / cam);
    std::ofstream mj(fs::path(dir) / cam / "0000.mjpeg", std::ios::binary);
    std::ofstream idx(fs::path(dir) / cam / "0000.csv");
    idx << "# frame,offset,size,width,height,jetson_us,robot_us\n";
    long long offset = 0;
    double far_x = 850;
    for (int f = 0; f < 180; ++f) {
      const double t = f / 60.0;
      cv::Mat img = Frame(rng);
      if (std::string(cam) == "CamA") {
        if (t < 1.0 || t >= 2.0) {
          DrawTag(img, 1, 70, 250, 300);
          DrawTag(img, 2, 70, 420, 330);
        }
        DrawTag(img, 7, 14, far_x, 400, 5);
        far_x += 1.0;
      }
      Finish(img, rng);
      auto jpeg = EncodeJpeg(img);
      mj.write(reinterpret_cast<const char *>(jpeg.data()), static_cast<std::streamsize>(jpeg.size()));
      // Cameras slightly out of step, as on the robot.
      const long long us = 1'000'000 + f * 16'667 + (std::string(cam) == "CamB" ? 4'000 : 0);
      idx << f << ',' << offset << ',' << jpeg.size() << ',' << kW << ',' << kH << ',' << us << ",0\n";
      offset += static_cast<long long>(jpeg.size());
    }
  }
  std::printf("wrote a synthetic session to %s (CamA, CamB: 180 frames each)\n", dir.c_str());
  return 0;
}

}  // namespace

int main(int argc, char **argv) {
  if (argc >= 3 && std::string(argv[1]) == "--write-session") {
    family = tag36h11_create();
    return WriteSession(argv[2]);
  }
  setvbuf(stdout, nullptr, _IOLBF, 0);  // line by line, so a timeout still shows how far it got
  family = tag36h11_create();
  apriltag_family_t *cam_family = tag36h11_create();
  apriltag_detector_t *td = MakeTd(cam_family);
  frc::apriltag::GpuDetector gpu(kW, kH, td, kCm, Dc(), vision::ImageFormat::MONO8);
  std::mt19937 rng(42);
  int64_t now_us = 1'000'000;
  const int64_t frame_us = 8197;  // 122 fps

  if (std::getenv("FST_PROBE")) {
    // Diagnostics: one full-size search on simple frames, timed.
    far_search::SetConfig({true, 1000});
    far_search::Process(9, Frame(rng), {}, td, kCm, Dc(), now_us);
    far_search::Forget(9);
    for (int kind = 0; kind < 4; ++kind) {
      cv::Mat img = Frame(rng);
      if (kind >= 2) DrawTag(img, 3, 30, 600, 400);
      if (kind == 1 || kind == 3) Finish(img, rng);
      now_us += 300'000;
      auto t0 = std::chrono::steady_clock::now();
      auto extra = far_search::Process(0, img, {}, td, kCm, Dc(), now_us);
      double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
      std::printf("  probe %s: %.1f ms, %zu tags\n", kind == 0 ? "flat" : kind == 1 ? "noise" : kind == 2 ? "tag, no noise" : "tag with noise", ms,
                  extra.size());
      far_search::Forget(0);
    }
    return 0;
  }
  std::cout << "== 1. Range: smallest tag found (tag 3, 7 deg)" << std::endl;
  far_search::SetConfig({true, 1000});  // every call may sweep, for this part
  // The first call ever counts as a good view (no search in the first 250 ms after start).
  far_search::Process(9, Frame(rng), {}, td, kCm, Dc(), now_us);
  far_search::Forget(9);
  double smallest_normal = 1e9, smallest_far = 1e9;
  for (double side : {40.0, 30.0, 24.0, 20.0, 18.0, 16.0, 14.0, 13.0, 12.0, 11.0, 10.0}) {
    int normal_hits = 0, far_hits = 0;
    const int kTrials = 4;
    for (int k = 0; k < kTrials; ++k) {
      cv::Mat img = Frame(rng);
      DrawTag(img, 3, side, 400 + 97 * k, 300 + 31 * k);
      Finish(img, rng);
      auto normal = DetectNormal(gpu, img);
      if (Found(normal, 3)) ++normal_hits;
      // Starve first (no good view for > 250 ms), then one call sweeps.
      now_us += 300'000;
      auto extra = far_search::Process(0, img, normal, td, kCm, Dc(), now_us);
      if (Found(normal, 3) || Found(extra, 3)) ++far_hits;
      far_search::Forget(0);
    }
    std::printf("  %4.0f px: normal %d/%d, with far search %d/%d\n", side, normal_hits, kTrials, far_hits, kTrials);
    if (normal_hits == kTrials) smallest_normal = std::min(smallest_normal, side);
    if (far_hits == kTrials) smallest_far = std::min(smallest_far, side);
  }
  std::printf("  smallest found every time: normal %.0f px, with far search %.0f px (%.2fx the range)\n",
              smallest_normal, smallest_far, smallest_normal / smallest_far);
  Check(smallest_far <= 14 && smallest_normal / smallest_far >= 1.4,
        "the far search finds tags at least 1.4x smaller (1.4x the range)");

  std::cout << "== 2. Corners: full-size search against the normal one (tags both find)" << std::endl;
  {
    double sum_dx = 0, sum_dy = 0, max_d = 0;
    int n = 0;
    for (int k = 0; k < 8; ++k) {
      cv::Mat img = Frame(rng);
      DrawTag(img, 5, 36 + 3 * k, 300 + 80 * k, 250 + 40 * k, 3 + 4 * k);
      Finish(img, rng);
      auto normal = DetectNormal(gpu, img);
      // The full search skips tags the normal search found, so ask it with the normal list empty.
      now_us += 300'000;
      auto full = far_search::Process(1, img, {}, td, kCm, Dc(), now_us);
      far_search::Forget(1);
      for (auto &a : normal) {
        for (auto &b : full) {
          if (a.id != b.id) continue;
          for (int i = 0; i < 8; i += 2) {
            const double dx = b.p[i] - a.p[i], dy = b.p[i + 1] - a.p[i + 1];
            sum_dx += dx;
            sum_dy += dy;
            max_d = std::max(max_d, std::hypot(dx, dy));
            ++n;
          }
        }
      }
    }
    const double mx = n ? sum_dx / n : 99, my = n ? sum_dy / n : 99;
    std::printf("  %d corners: mean offset (%.3f, %.3f) px, largest %.3f px\n", n, mx, my, max_d);
    Check(n >= 16 && std::abs(mx) < 0.15 && std::abs(my) < 0.15 && max_d < 0.8,
          "the full-size search's corners land where the normal search's do");
  }

  std::cout << "== 3. Policy: good view, starved, tracking, good again (2 cameras)" << std::endl;
  far_search::SetConfig({true, 30});
  for (int c = 0; c < 2; ++c) far_search::Forget(c);
  const auto before = far_search::GetCounters();
  int phase_a_extra = 0, sweeps_a = 0;
  int64_t starved_at = -1, first_far_at = -1, gone_at = -1;
  int frames_b = 0, found_b = 0, frames_after_first = 0, found_after_first = 0, normal_b = 0;
  int phase_c_extra = 0;
  double far_x = 900;
  for (int f = 0; f < 3 * 122; ++f) {  // 3 s at 122 fps
    const double t = f / 122.0;
    const bool near = t < 1.0 || t >= 2.0;  // near tags in view, then not, then again
    for (int c = 0; c < 2; ++c) {
      now_us += frame_us / 2;
      cv::Mat img = Frame(rng);
      if (c == 0) {
        if (near) {
          DrawTag(img, 1, 70, 250, 300);
          DrawTag(img, 2, 70, 420, 330);
        }
        DrawTag(img, 7, 14, far_x, 400, 5);  // the far tag, too small for the normal search
      }
      Finish(img, rng);
      auto normal = DetectNormal(gpu, img);
      const auto c0 = far_search::GetCounters();
      auto extra = far_search::Process(c, img, normal, td, kCm, Dc(), now_us);
      if (c != 0) continue;
      if (t < 1.0) {
        phase_a_extra += static_cast<int>(extra.size());
        sweeps_a += static_cast<int>(far_search::GetCounters().sweeps - c0.sweeps);
      } else if (t < 2.0) {
        if (gone_at < 0) gone_at = now_us;
        if (starved_at < 0 && far_search::GetCounters().starved) starved_at = now_us;
        ++frames_b;
        normal_b += Found(normal, 7);
        const bool got = Found(extra, 7) || Found(normal, 7);  // either search, as PhotonVision sees it
        found_b += got;
        if (Found(extra, 7) && first_far_at < 0) first_far_at = now_us;
        if (first_far_at >= 0) {
          ++frames_after_first;
          found_after_first += got;
        }
        far_x += 1.5;  // it moves: crops have to follow it
      } else {
        phase_c_extra += static_cast<int>(extra.size());
      }
    }
  }
  const auto after = far_search::GetCounters();
  std::printf("  good view (1 s): %d extra detections, %d full-size searches\n", phase_a_extra, sweeps_a);
  Check(phase_a_extra == 0 && sweeps_a == 0, "nothing extra while a camera has a good view");
  std::printf("  near tags gone: starved after %.0f ms, far tag first found %.0f ms after they went; found on "
              "%d/%d frames after that (%.0f%%)\n",
              starved_at < 0 ? -1.0 : (starved_at - gone_at) / 1000.0,
              first_far_at < 0 ? -1.0 : (first_far_at - gone_at) / 1000.0, found_after_first, frames_after_first,
              frames_after_first ? 100.0 * found_after_first / frames_after_first : 0);
  Check(starved_at >= 0 && starved_at - gone_at >= 240'000 && starved_at - gone_at <= 280'000,
        "starved about 250 ms after the good view ends");
  std::printf("  (the normal search alone found the 14 px tag on %d/%d frames)\n", normal_b, frames_b);
  Check(first_far_at >= 0, "the far tag is found once the cameras are starved");
  Check(frames_after_first > 0 && found_after_first >= 0.9 * frames_after_first,
        "crops keep finding it on at least 90% of frames as it moves");
  std::printf("  good view again (1 s): %d extra detections\n", phase_c_extra);
  Check(phase_c_extra == 0, "everything stops as soon as a good view returns");
  const long sweeps = after.sweeps - before.sweeps;
  std::printf("  full-size searches: %ld in ~1 s starved (budget 30/s); %.2f ms each; crops %.2f ms each\n", sweeps,
              sweeps ? (after.sweep_ms - before.sweep_ms) / sweeps : 0,
              after.crops - before.crops ? (after.crop_ms - before.crop_ms) / (after.crops - before.crops) : 0);
  // Starved for ~0.75 s of the middle second (the first 250 ms is the wait): ~22 at 30 a second.
  Check(sweeps >= 15 && sweeps <= 26, "full-size searches kept to the budget (about 22 in 0.75 s starved)");

  std::cout << "== 4. Off" << std::endl;
  far_search::SetConfig({false, 30});
  {
    int extra_n = 0;
    for (int f = 0; f < 122; ++f) {
      now_us += frame_us;
      cv::Mat img = Frame(rng);
      DrawTag(img, 7, 14, 900, 400, 5);
      Finish(img, rng);
      auto normal = DetectNormal(gpu, img);
      extra_n += static_cast<int>(far_search::Process(0, img, normal, td, kCm, Dc(), now_us).size());
    }
    Check(extra_n == 0, "with the far search off, nothing extra");
  }

  std::printf("\n%s: %d check(s) failed\n", failures ? "FAIL" : "PASS", failures);
  cudaDeviceSynchronize();
  return failures ? 1 : 0;
}
