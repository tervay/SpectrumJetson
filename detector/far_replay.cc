// far_replay: replays a whole Rewind session (every camera, merged in recording-time order) through
// the 971 detector, as PhotonVision runs it, with the far-tag search (far_search.h) deciding on each
// frame exactly as it does live: its good-view / starved policy spans all cameras, so this replays
// them together, on the recording's own clock.
//
//   far_replay SESSION_DIR [--out OUT.csv] [--budget N] [--off] [--every N] [--threads N]
//              [--calib CAMERA=fx,fy,cx,cy,k1,k2,p1,p2,k3,k4,k5,k6]... [--mwbd N] [--mse X]
//
// SESSION_DIR: a Rewind session (one folder per camera, docs/REWIND.md). --budget: full-size
// searches a second (default 30, as live). --off: the far search switched off, for a baseline.
// --every N: every Nth frame of each camera (the policy's timings stay on the recording's clock).
// --calib: a camera's lens calibration by folder name; without one, no undistortion (as in
// PhotonVision before a calibration).
//
// OUT.csv, one row per detection: camera,frame,jetson_us,id,margin,side_px,source,x0,y0,...,x3,y3
// (source: normal or far). Summary on stdout: tag sightings (camera x frame x tag) by the normal
// search and with the far search, the tags only the far search found and how small they were,
// time starved, full-size searches and crops, and what they cost, and the slowest frames.
// --trace: each frame's far-search time on stderr as it goes (to find a frame that stalls).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <cuda_runtime.h>
#include <opencv2/core/mat.hpp>

#include "absl/status/status.h"
#include "apriltag/apriltag.h"
#include "apriltag/tag36h11.h"
#include "far_search.h"
#include "rewind_reader.h"
#include "third_party/971apriltag/apriltag.h"

namespace fs = std::filesystem;

namespace {

struct Camera {
  std::string name;
  std::vector<recording::Frame> frames;
  int width = 0, height = 0;
  apriltag_family_t *family = nullptr;
  apriltag_detector_t *td = nullptr;
  frc::apriltag::GpuDetector *gpu = nullptr;
  frc::apriltag::CameraMatrix cm{1, 1, 1, 1};  // as the live wrapper before a calibration
  frc::apriltag::DistCoeffs dc{};
  // Results
  long frames_done = 0, normal_sightings = 0, far_sightings = 0, bad_jpegs = 0;
  std::map<int, long> far_only;           // tag -> frames only the far search found it
  std::map<int, double> far_only_min_px;  // tag -> smallest side it was found at
};

struct Item {
  int camera;
  size_t frame;
  long long jetson_us;
};

double EnvOr(const char *name, double fallback) {
  const char *v = std::getenv(name);
  return v && *v ? std::atof(v) : fallback;
}

}  // namespace

int main(int argc, char **argv) {
  setvbuf(stdout, nullptr, _IOLBF, 0);
  if (argc < 2) {
    std::cerr << "usage: far_replay SESSION_DIR [--out OUT.csv] [--budget N] [--off] [--every N] [--threads N]"
                 " [--calib CAMERA=fx,fy,cx,cy,k1,k2,p1,p2,k3,k4,k5,k6]... [--mwbd N] [--mse X]\n";
    return 2;
  }
  const fs::path session = fs::path(argv[1]).lexically_normal().string().back() == '/'
                               ? fs::path(argv[1]).lexically_normal().parent_path()
                               : fs::path(argv[1]).lexically_normal();
  std::string out_path;
  double budget = 30;
  bool off = false, trace = false;
  int every = 1, threads = 5;
  double mwbd = EnvOr("SPECTRUM_971_MIN_WHITE_BLACK_DIFF", 5), mse = EnvOr("SPECTRUM_971_MAX_LINE_FIT_MSE", 10);
  std::map<std::string, std::vector<double>> calibs;
  for (int i = 2; i < argc; ++i) {
    const std::string a = argv[i];
    const char *v = i + 1 < argc ? argv[i + 1] : "";
    if (a == "--out") out_path = v, ++i;
    else if (a == "--budget") budget = std::atof(v), ++i;
    else if (a == "--off") off = true;
    else if (a == "--trace") trace = true;
    else if (a == "--every") every = std::max(1, std::atoi(v)), ++i;
    else if (a == "--threads") threads = std::max(1, std::atoi(v)), ++i;
    else if (a == "--mwbd") mwbd = std::atof(v), ++i;
    else if (a == "--mse") mse = std::atof(v), ++i;
    else if (a == "--calib") {
      const std::string c = v;
      const auto eq = c.find('=');
      std::vector<double> k;
      std::stringstream ss(eq == std::string::npos ? "" : c.substr(eq + 1));
      std::string t;
      while (std::getline(ss, t, ',')) k.push_back(std::atof(t.c_str()));
      if (eq == std::string::npos || k.size() < 4) {
        std::cerr << "--calib needs CAMERA=fx,fy,cx,cy[,k1,k2,p1,p2,k3,k4,k5,k6]\n";
        return 2;
      }
      calibs[c.substr(0, eq)] = k;
      ++i;
    } else {
      std::cerr << "unknown option " << a << "\n";
      return 2;
    }
  }

  // Every camera folder with frames.
  std::vector<Camera> cams;
  for (auto &e : fs::directory_iterator(session)) {
    if (!e.is_directory()) continue;
    auto frames = recording::ReadIndex(e.path());
    if (frames.empty()) continue;
    Camera c;
    c.name = e.path().filename().string();
    for (size_t i = 0; i < frames.size(); i += every) c.frames.push_back(frames[i]);
    c.width = c.frames[0].width;
    c.height = c.frames[0].height;
    cams.push_back(std::move(c));
  }
  std::sort(cams.begin(), cams.end(), [](const Camera &a, const Camera &b) { return a.name < b.name; });
  if (cams.empty() || static_cast<int>(cams.size()) > far_search::kMaxCameras) {
    std::cerr << "no camera folders with frames in " << session << " (or more than " << far_search::kMaxCameras
              << ")\n";
    return 1;
  }

  // A detector per camera, with PhotonVision's settings (GpuDetectorJNI.cc's MakeTagDetector).
  for (auto &c : cams) {
    c.family = tag36h11_create();
    c.td = apriltag_detector_create();
    apriltag_detector_add_family_bits(c.td, c.family, 1);
    c.td->nthreads = 6;
    c.td->wp = workerpool_create(c.td->nthreads);
    c.td->qtp.min_white_black_diff = static_cast<int>(mwbd);
    c.td->qtp.max_line_fit_mse = static_cast<float>(mse);
    c.td->debug = false;
    c.dc.num_params = 5;
    if (auto it = calibs.find(c.name); it != calibs.end()) {
      const auto &k = it->second;
      c.cm = frc::apriltag::CameraMatrix{k[0], k[2], k[1], k[3]};  // {fx, cx, fy, cy}
      double *d[] = {&c.dc.k1, &c.dc.k2, &c.dc.p1, &c.dc.p2, &c.dc.k3, &c.dc.k4, &c.dc.k5, &c.dc.k6};
      for (size_t j = 4; j < k.size() && j - 4 < 8; ++j) *d[j - 4] = k[j];
      if (k.size() >= 12) c.dc.num_params = 8;  // PhotonVision's 8-coefficient calibrations
    }
    c.gpu = new frc::apriltag::GpuDetector(c.width, c.height, c.td, c.cm, c.dc, vision::ImageFormat::MONO8);
  }

  // All frames, merged in recording-time order.
  std::vector<Item> items;
  for (int ci = 0; ci < static_cast<int>(cams.size()); ++ci)
    for (size_t fi = 0; fi < cams[ci].frames.size(); ++fi) items.push_back({ci, fi, cams[ci].frames[fi].jetson_us});
  std::sort(items.begin(), items.end(), [](const Item &a, const Item &b) { return a.jetson_us < b.jetson_us; });
  const double seconds = items.size() > 1 ? (items.back().jetson_us - items.front().jetson_us) / 1e6 : 0;

  std::printf("%s: %zu cameras, %zu frames, %.1f s; far search %s (%.0f full-size searches a second)\n",
              session.filename().c_str(), cams.size(), items.size(), seconds, off ? "OFF" : "on", budget);
  for (auto &c : cams)
    std::printf("  %s: %zu frames %dx%d%s\n", c.name.c_str(), c.frames.size(), c.width, c.height,
                calibs.count(c.name) ? ", calibrated" : "");
  far_search::SetConfig({!off, budget});

  std::ofstream out;
  if (!out_path.empty()) {
    out.open(out_path);
    out << "camera,frame,jetson_us,id,margin,side_px,source,x0,y0,x1,y1,x2,y2,x3,y3\n";
  }

  // Decode ahead in batches on CPU threads; detect in order on the GPU.
  const size_t batch = static_cast<size_t>(threads) * 8;
  std::vector<std::vector<uint8_t>> gray(batch);
  std::vector<char> ok(batch);
  auto t_start = std::chrono::steady_clock::now();
  double detect_ms = 0, far_ms = 0;
  // The slowest frames for the far search: a full-size search can't be cut short, so one slow scene
  // is one camera's frame late. Kept to report the worst.
  struct Slow {
    double ms;
    std::string camera;
    int frame;
  };
  std::vector<Slow> slow;
  for (size_t b0 = 0; b0 < items.size(); b0 += batch) {
    const size_t n = std::min(batch, items.size() - b0);
    std::atomic<size_t> next{0};
    std::vector<std::thread> workers;
    for (int t = 0; t < threads; ++t) {
      workers.emplace_back([&] {
        std::vector<unsigned char> jpeg;
        for (size_t k = next++; k < n; k = next++) {
          const Item &it = items[b0 + k];
          const Camera &c = cams[it.camera];
          ok[k] = recording::ReadJpeg(c.frames[it.frame], jpeg) && recording::DecodeGray(jpeg, gray[k], c.width, c.height);
        }
      });
    }
    for (auto &w : workers) w.join();
    for (size_t k = 0; k < n; ++k) {
      const Item &it = items[b0 + k];
      Camera &c = cams[it.camera];
      if (!ok[k]) {
        ++c.bad_jpegs;
        continue;
      }
      cv::Mat img(c.height, c.width, CV_8UC1, gray[k].data());
      auto t0 = std::chrono::steady_clock::now();
      if (!c.gpu->Detect(gray[k].data(), nullptr).ok()) continue;
      auto normal = far_search::Copy(c.gpu->Detections());
      auto t1 = std::chrono::steady_clock::now();
      if (trace) std::fprintf(stderr, "trace %s frame %d...", c.name.c_str(), c.frames[it.frame].index);
      auto far = far_search::Process(it.camera, img, normal, c.td, c.cm, c.dc, it.jetson_us);
      if (trace)
        std::fprintf(stderr, " %.1f ms\n",
                     std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count());
      auto t2 = std::chrono::steady_clock::now();
      detect_ms += std::chrono::duration<double, std::milli>(t1 - t0).count();
      const double this_far_ms = std::chrono::duration<double, std::milli>(t2 - t1).count();
      far_ms += this_far_ms;
      if (this_far_ms > 20) slow.push_back({this_far_ms, c.name, c.frames[it.frame].index});
      ++c.frames_done;
      // As PhotonVision keeps them: decision margin 15 or more.
      std::set<int> normal_ids;
      for (const auto &d : normal)
        if (d.margin >= 15) normal_ids.insert(d.id);
      c.normal_sightings += static_cast<long>(normal_ids.size());
      std::set<int> all_ids = normal_ids;
      for (const auto &d : far) {
        if (d.margin < 15) continue;
        if (all_ids.insert(d.id).second) {
          c.far_only[d.id]++;
          auto m = c.far_only_min_px.find(d.id);
          if (m == c.far_only_min_px.end() || d.side() < m->second) c.far_only_min_px[d.id] = d.side();
        }
      }
      c.far_sightings += static_cast<long>(all_ids.size());
      if (out) {
        auto row = [&](const far_search::Det &d, const char *src) {
          out << c.name << ',' << c.frames[it.frame].index << ',' << it.jetson_us << ',' << d.id << ',' << d.margin << ','
              << d.side() << ',' << src;
          for (double v : d.p) out << ',' << v;
          out << '\n';
        };
        for (const auto &d : normal)
          if (d.margin >= 15) row(d, "normal");
        for (const auto &d : far)
          if (d.margin >= 15) row(d, "far");
      }
    }
    if ((b0 / batch) % 50 == 49) {
      std::printf("  ... %zu of %zu frames\n", b0 + n, items.size());
    }
  }
  const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();

  const auto fc = far_search::GetCounters();
  long normal_total = 0, far_total = 0, frames_total = 0;
  std::printf("\nPer camera (tag sightings: frames x tags, decision margin 15+):\n");
  for (auto &c : cams) {
    normal_total += c.normal_sightings;
    far_total += c.far_sightings;
    frames_total += c.frames_done;
    std::printf("  %-12s %6ld frames: %6ld sightings by the normal search, %6ld with the far search (+%ld)%s\n",
                c.name.c_str(), c.frames_done, c.normal_sightings, c.far_sightings,
                c.far_sightings - c.normal_sightings, c.bad_jpegs ? " (some bad JPEGs skipped)" : "");
    for (auto &[id, count] : c.far_only)
      std::printf("      tag %d: %ld frames only the far search found it (smallest %.1f px)\n", id, count,
                  c.far_only_min_px[id]);
  }
  std::printf("\nTotal: %ld sightings normal, %ld with the far search (+%.1f%%)\n", normal_total, far_total,
              normal_total ? 100.0 * (far_total - normal_total) / normal_total : 0);
  std::printf("Far search: starved %.1f s of %.1f s; %ld full-size searches (%.2f ms each), %ld crops (%.2f ms each)\n",
              fc.starved_ms / 1000.0, seconds, fc.sweeps, fc.sweeps ? fc.sweep_ms / fc.sweeps : 0, fc.crops,
              fc.crops ? fc.crop_ms / fc.crops : 0);
  std::printf("Time: detect %.2f ms a frame, far search %.2f ms a frame on average; replay took %.0f s\n",
              frames_total ? detect_ms / frames_total : 0, frames_total ? far_ms / frames_total : 0, wall);
  std::sort(slow.begin(), slow.end(), [](const Slow &a, const Slow &b) { return a.ms > b.ms; });
  std::printf("Frames where the far search took over 20 ms: %zu", slow.size());
  for (size_t i = 0; i < slow.size() && i < 5; ++i)
    std::printf("%s %s frame %d %.0f ms", i ? "," : ";", slow[i].camera.c_str(), slow[i].frame, slow[i].ms);
  std::printf("\n");
  cudaDeviceSynchronize();
  return 0;
}
