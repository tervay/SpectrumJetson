// JNI bridge between the FRC-Team-4143 PhotonVision fork (org.photonvision.jni.GpuDetectorJNI)
// and Austin Schuh's current CUDA AprilTag detector, as built in frc971/bos
// third_party/971apriltag (from RealtimeRoboticsGroup/aos frc/orin).
//
// Keeps the exact Java API of FRC-Team-4143/GpuDetectorJNI so the fork jar is unchanged:
//   long createGpuDetector(int width, int height)
//   void destroyGpuDetector(long handle)
//   void setparams(long handle, fx, cx, fy, cy, k1, k2, p1, p2, k3)
//   void setparams8(long handle, fx, cx, fy, cy, k1, k2, p1, p2, k3, k4, k5, k6)   // ours
//   AprilTagDetection[] processimage(long handle, long cvMatPtr)   // 8-bit mono Mat
//
// Differences from the 4143 JNI (see SpectrumJetson patches/gpudetector-0*.patch for the
// same fixes applied to the old code):
//   - detector slots are reused after destroy; every handle is bounds-checked
//   - detectors are freed with apriltag_detector_destroy / tag36h11_destroy
//   - a per-slot mutex stops destroy/setparams racing processimage
//   - stale CUDA errors are cleared before each detect (NVIDIA/cccl#1791)
//   - Detect() failures (absl::Status) are logged and return no detections
//   - CUDA errors throw (patches/bos-01-nonfatal-cuda.patch) instead of aborting the JVM:
//     the frame is skipped and the detector rebuilt on the next frame; only after
//     the CUDA context stays broken (sticky error) for kMaxFailingTime do we exit so systemd restarts
//     PhotonVision (a broken CUDA context cannot be recovered in-process)
//   - no per-detection std::cout; a once-per-second stats line instead

#include <jni.h>
#include <wpi/jni_util.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <mutex>
#include <pthread.h>
#include <shared_mutex>
#include <string>
#include <thread>
#include <vector>

#include <csetjmp>

#include <dlfcn.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <cuda_runtime.h>
#include <jpeglib.h>
#include "far_search.h"
#include "nvjpg_decoder.h"
#include <wpi/RawFrame.h>
#include <wpi/timestamp.h>
#include "absl/status/status.h"
#include <opencv2/core/mat.hpp>

#include "apriltag/apriltag.h"
#include "apriltag/tag36h11.h"
#include "third_party/971apriltag/apriltag.h"

void SpectrumInjectStickyCudaFault();  // fault_kernel.cu (test only)

namespace {

wpi::java::JClass detectionCls;

const wpi::java::JClassInit classes[] = {
    {"edu/wpi/first/apriltag/AprilTagDetection", &detectionCls}};

// Contrast threshold for the GPU thresholding step. 4143 used 5, frc971/bos uses 4,
// and RealtimeRoboticsGroup/aos 76d8f216 moved to 20 for speed. Override at launch
// with SPECTRUM_971_MIN_WHITE_BLACK_DIFF.
int MinWhiteBlackDiff() {
  if (const char *v = std::getenv("SPECTRUM_971_MIN_WHITE_BLACK_DIFF")) return std::atoi(v);
  return 5;
}

// Largest mean-squared error of a candidate quad's edge-line fit. AprilTag's default is 10.
// Upstream PhotonVision PR #2138 lowers its CPU detector to 2.5, which stops tags cut off at the
// image edge from being detected, with little effect on range in their tests. Set with
// SPECTRUM_971_MAX_LINE_FIT_MSE (08-select-detector.sh --mse N); test range and edge behaviour
// before changing it.
float MaxLineFitMse() {
  if (const char *v = std::getenv("SPECTRUM_971_MAX_LINE_FIT_MSE")) {
    const float f = std::strtof(v, nullptr);
    if (f > 0) return f;
  }
  return 10.0f;
}

// Worker threads for each detector's CPU stage (edge refinement and tag decoding). Every camera
// has its own detector and pool, so 4 cameras x 6 threads compete for the Jetson's 6 cores. Set
// with SPECTRUM_971_THREADS; /tmp/spectrum-971-threads overrides it without a restart
// (re-read every 2 s; processimage swaps the pool between frames), for A/B tests.
int DetectorThreads() {
  static std::mutex mu;
  static std::chrono::steady_clock::time_point next{};
  static int threads = 6;
  std::lock_guard<std::mutex> lock(mu);
  const auto now = std::chrono::steady_clock::now();
  if (now >= next) {
    next = now + std::chrono::seconds(2);
    int n = 6;
    if (const char *e = std::getenv("SPECTRUM_971_THREADS")) n = std::atoi(e);
    if (FILE *f = std::fopen("/tmp/spectrum-971-threads", "r")) {
      if (std::fscanf(f, "%d", &n) != 1) n = 6;
      std::fclose(f);
    }
    threads = n >= 1 && n <= 12 ? n : 6;
  }
  return threads;
}

// One camera's Detect() at a time. All cameras share one CUDA context, and libcuda guards it with
// one lock (a priority-inheritance mutex) that every launch and event wait takes. With 4 cameras
// at 120 fps (2026-09-29) the threads convoyed on it: ~20,000 cross-core wakeups a second, 1.2
// cores of kernel time, detect 4.4 ms against 1.5 ms for one camera, and the GPU ~40% "busy"
// against ~22% for 3 cameras. Taking this lock first makes each camera queue once per frame
// instead of ~40 times. SPECTRUM_971_GPU_LOCK=1/0; /tmp/spectrum-971-gpu-lock overrides it
// (re-read every 2 s, for A/B tests).
bool GpuLockOn() {
  static std::mutex mu;
  static std::chrono::steady_clock::time_point next{};
  static bool on = false;
  std::lock_guard<std::mutex> lock(mu);
  const auto now = std::chrono::steady_clock::now();
  if (now >= next) {
    next = now + std::chrono::seconds(2);
    int v = 0;
    if (const char *e = std::getenv("SPECTRUM_971_GPU_LOCK")) v = std::atoi(e);
    if (FILE *f = std::fopen("/tmp/spectrum-971-gpu-lock", "r")) {
      if (std::fscanf(f, "%d", &v) != 1) v = 0;
      std::fclose(f);
    }
    if ((v != 0) != on) std::cout << "971 GPU lock: " << (v ? "on" : "off") << std::endl;
    on = v != 0;
  }
  return on;
}
std::mutex gpu_mu;

// CUDA stream capture (bos-05's first-stage graph) breaks CUDA calls on other threads that aren't
// allowed while any stream is capturing: work on the legacy default stream, cudaFree, EGL buffer
// registration. When all cameras started at once, that broke detectors and turned the hardware
// JPEG decoder off on 4 of 10 starts (2026-09-29). So every CUDA path in this process holds this
// lock shared (one atomic operation when uncontended), and a graph is recorded holding it
// exclusively, once per detector. Writer-preferring: with 8 threads taking it shared all the
// time, glibc's default would let a waiting recorder starve. spectrum_cuda_lock_shared/unlock
// export it for libspectrumtrt_jni.so (TensorRT game pieces).
class CudaCaptureLock {
 public:
  CudaCaptureLock() {
    pthread_rwlockattr_t a;
    pthread_rwlockattr_init(&a);
    pthread_rwlockattr_setkind_np(&a, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
    pthread_rwlock_init(&lock_, &a);
    pthread_rwlockattr_destroy(&a);
  }
  void lock_shared() { pthread_rwlock_rdlock(&lock_); }
  void unlock_shared() { pthread_rwlock_unlock(&lock_); }
  void lock() { pthread_rwlock_wrlock(&lock_); }
  void unlock() { pthread_rwlock_unlock(&lock_); }

 private:
  pthread_rwlock_t lock_;
};
CudaCaptureLock cuda_capture_lock;

// How the CPU thread waits for the GPU (cudaSetDeviceFlags, at library load, before any CUDA
// context exists): "block" (sleep; the default), "auto" (CUDA's default; with one context on 6
// cores it spins), "spin" or "yield". With 2 cameras (2026-09-24) blocking added ~0.3 ms and saved
// no CPU. With 4 cameras it made no difference over 11 one-minute runs (docs/TECHNICAL.md), so it's
// the default because it doesn't hurt. Set with SPECTRUM_971_CUDA_SYNC;
// /tmp/spectrum-971-cuda-sync overrides it (for A/B tests: write it, restart PhotonVision).
unsigned CudaScheduleFlag(std::string *name) {
  std::string v = "block";
  if (const char *e = std::getenv("SPECTRUM_971_CUDA_SYNC")) v = e;
  if (FILE *f = std::fopen("/tmp/spectrum-971-cuda-sync", "r")) {
    char buf[16] = {0};
    if (std::fgets(buf, sizeof(buf), f)) v = std::string(buf).substr(0, std::strcspn(buf, " \r\n"));
    std::fclose(f);
  }
  *name = v;
  if (v == "spin") return cudaDeviceScheduleSpin;
  if (v == "yield") return cudaDeviceScheduleYield;
  if (v == "auto") return cudaDeviceScheduleAuto;
  *name = "block";
  return cudaDeviceScheduleBlockingSync;
}

// Test hooks, read from /tmp/spectrum-971-fault-every (re-read every 30 frames so a test can
// remove it before a restarted process runs):
//   N > 0     every Nth frame hits a real, non-sticky CUDA error (cudaSetDevice on a bad
//             ordinal right before Detect: the NVIDIA/cccl#1791 case) -> frame skipped
//   "sticky"  one null-pointer kernel write: a sticky error that breaks the CUDA context
//             -> the watchdog restarts PhotonVision
constexpr const char *kFaultFile = "/tmp/spectrum-971-fault-every";
constexpr int kFaultSticky = -1;
int FaultEvery() {
  static int n = 0;
  static int calls = 0;
  if (calls++ % 30 == 0) {
    n = 0;
    if (FILE *f = std::fopen(kFaultFile, "r")) {
      char buf[16] = {0};
      if (std::fgets(buf, sizeof(buf), f)) n = (std::strncmp(buf, "sticky", 6) == 0) ? kFaultSticky : std::atoi(buf);
      std::fclose(f);
    }
  }
  return n;
}

frc::apriltag::CameraMatrix DefaultCameraMatrix() {
  return frc::apriltag::CameraMatrix{1, 1, 1, 1};
}

frc::apriltag::DistCoeffs DefaultDistCoeffs() {
  frc::apriltag::DistCoeffs d{};
  d.num_params = 5;
  return d;
}

struct Stats {
  std::chrono::steady_clock::time_point start{};
  int frames = 0;
  int tags = 0;
  int errors = 0;
  double detect_ms = 0, jni_ms = 0, max_ms = 0, margin = 0, min_margin = 1e9;
  double lock_wait_ms = 0;  // part of detect_ms spent waiting for gpu_mu
  int gpu_input = 0;        // frames Detect() got straight from the hardware decoder's GPU copy
  double age_ms = 0;        // capture (first USB packet) to the end of detection, summed
  int ages = 0;
};

struct DetectorSlot {
  frc::apriltag::GpuDetector *gpu = nullptr;
  apriltag_detector_t *td = nullptr;
  apriltag_family_t *family = nullptr;
  frc::apriltag::CameraMatrix camera_matrix = DefaultCameraMatrix();
  frc::apriltag::DistCoeffs dist_coeffs = DefaultDistCoeffs();
  long gpu_input_frames = 0;  // for the GPU input check
  // When the first-stage graph (bos-05) was last recorded: at most once a second, so a buffer
  // or mask that keeps changing can't stall every camera on each frame.
  std::chrono::steady_clock::time_point last_graph_record{};
  // Detection mask (setMask, bos-07): 0 off, 1 ignore inside the boxes, 2 search only inside
  // them. Boxes are x, y, w, h fractions of the image. mask_dirty: rasterise it on the next frame.
  int mask_mode = 0;
  std::vector<double> mask_rects;
  bool mask_dirty = false;
  std::vector<uint8_t> mask_pixels;  // the decimated mask handed to the GPU (kept alive)
  bool in_use = false;
  bool needs_rebuild = false;
  int consecutive_failures = 0;
  std::chrono::steady_clock::time_point first_failure{};
  std::mutex mu;
  Stats stats;
};

// A broken CUDA context (sticky error) for this long, over at least kMinFailures frames,
// means only a process restart can recover. Time-based because each failed frame also
// rebuilds the detector, so frame count alone stretched this to ~5 s.
constexpr std::chrono::milliseconds kMaxFailingTime{1000};
constexpr int kMinFailures = 3;

constexpr int kMaxDetectors = 10;
DetectorSlot slots[kMaxDetectors];
std::mutex alloc_mu;

DetectorSlot *Slot(jlong handle) {
  if (handle < 0 || handle >= kMaxDetectors) return nullptr;
  if (!slots[handle].in_use) return nullptr;
  return &slots[handle];
}

apriltag_detector_t *MakeTagDetector(apriltag_family_t *family) {
  apriltag_detector_t *td = apriltag_detector_create();
  apriltag_detector_add_family_bits(td, family, 1);
  td->nthreads = DetectorThreads();
  td->wp = workerpool_create(td->nthreads);
  td->qtp.min_white_black_diff = MinWhiteBlackDiff();
  td->qtp.max_line_fit_mse = MaxLineFitMse();
  td->debug = false;
  // GpuDetector CHECKs these (the AprilTag defaults): quad_decimate 2, no deglitch.
  return td;
}

// Rebuilds the GPU detector for a new size or calibration. Caller holds s.mu.
// The slot's mask at the detector's decimated size (1 = search, 0 = ignore), handed to the GPU;
// or none. An "only inside" mask with no boxes counts as no mask, so an empty list can't switch
// detection off without anyone noticing.
void ApplyMaskToDetector(DetectorSlot &s, int width, int height) {
  s.mask_dirty = false;
  const size_t n_rects = s.mask_rects.size() / 4;
  if (!s.gpu || s.mask_mode == 0 || (s.mask_mode == 1 && n_rects == 0) ||
      (s.mask_mode == 2 && n_rects == 0)) {
    if (s.gpu) s.gpu->SetMask(nullptr);
    return;
  }
  const int dw = width / 2, dh = height / 2;
  const bool include = s.mask_mode == 2;
  s.mask_pixels.assign(static_cast<size_t>(dw) * dh, include ? 0 : 1);
  for (size_t r = 0; r < n_rects; ++r) {
    const double *b = &s.mask_rects[r * 4];
    const int x0 = std::clamp(static_cast<int>(std::floor(b[0] * dw)), 0, dw);
    const int y0 = std::clamp(static_cast<int>(std::floor(b[1] * dh)), 0, dh);
    const int x1 = std::clamp(static_cast<int>(std::ceil((b[0] + b[2]) * dw)), 0, dw);
    const int y1 = std::clamp(static_cast<int>(std::ceil((b[1] + b[3]) * dh)), 0, dh);
    for (int y = y0; y < y1; ++y) {
      std::fill(s.mask_pixels.begin() + static_cast<size_t>(y) * dw + x0,
                s.mask_pixels.begin() + static_cast<size_t>(y) * dw + x1, include ? 1 : 0);
    }
  }
  s.gpu->SetMask(s.mask_pixels.data());
}

bool Rebuild(DetectorSlot &s, size_t width, size_t height) {
  delete s.gpu;
  s.gpu = nullptr;
  try {
    s.gpu = new frc::apriltag::GpuDetector(width, height, s.td, s.camera_matrix,
                                           s.dist_coeffs, vision::ImageFormat::MONO8);
    s.needs_rebuild = false;
    s.mask_dirty = true;  // a new detector has no mask yet
    return true;
  } catch (const std::exception &e) {
    std::cout << "971 detector build " << width << "x" << height << " failed: " << e.what()
              << std::endl;
    return false;
  }
}

// Called with s.mu held after a failed frame or rebuild. The frame is skipped and the detector
// rebuilt next frame. PhotonVision is restarted only if the CUDA *context* is broken: a sticky
// error (illegal address, launch failure) makes every later call fail, including
// cudaDeviceSynchronize, and only a new process recovers. A failure on one camera with a
// healthy context must never take the other cameras down or cause a restart loop.
void RecordFailure(DetectorSlot &s, jlong handle, const char *what) {
  s.needs_rebuild = true;
  auto now = std::chrono::steady_clock::now();
  const bool context_ok = cudaDeviceSynchronize() == cudaSuccess;
  cudaGetLastError();  // clear whatever was pending so the rebuild starts clean
  if (s.consecutive_failures == 0 || context_ok) s.first_failure = now;
  if (++s.consecutive_failures <= 5 || s.consecutive_failures % 30 == 0) {
    std::cout << "971 detector h" << handle << " failure " << s.consecutive_failures << ": "
              << what << (context_ok ? " (CUDA context OK; frame skipped)" : " (CUDA context BROKEN)")
              << std::endl;
  }
  if (!context_ok && s.consecutive_failures >= kMinFailures &&
      now - s.first_failure >= kMaxFailingTime) {
    std::cout << "971 detector h" << handle << ": CUDA context broken for "
              << std::chrono::duration<double>(now - s.first_failure).count()
              << " s; exiting so systemd restarts PhotonVision" << std::endl;
    // _exit, not abort(): SIGABRT runs the JVM crash handler and Apport, which took ~28 s
    // (and a 156 MB /var/crash report) before the process died. Exit code 1 still makes
    // systemd's Restart=on-failure restart it.
    std::_Exit(1);
  }
}

jobject MakeJObject(JNIEnv *env, const apriltag_detection_t *detect) {
  static jmethodID constructor =
      env->GetMethodID(detectionCls, "<init>", "(Ljava/lang/String;IIF[DDD[D)V");
  if (!constructor) return nullptr;

  wpi::java::JLocal<jstring> fam{env, wpi::java::MakeJString(env, detect->family->name)};
  auto homography = detect->H;
  wpi::java::JLocal<jdoubleArray> harr{
      env, wpi::java::MakeJDoubleArray(
               env, {reinterpret_cast<const jdouble *>(homography->data),
                     static_cast<size_t>(homography->nrows * homography->ncols)})};
  wpi::java::JLocal<jdoubleArray> carr{
      env, wpi::java::MakeJDoubleArray(
               env, {reinterpret_cast<const jdouble *>(detect->p), 4 * 2})};

  return env->NewObject(detectionCls, constructor, fam.obj(), static_cast<jint>(detect->id),
                        static_cast<jint>(detect->hamming),
                        static_cast<jfloat>(detect->decision_margin), harr.obj(),
                        static_cast<jdouble>(detect->c[0]), static_cast<jdouble>(detect->c[1]),
                        carr.obj());
}

// SpectrumJetson: the same from a copied detection (far_search.h), for results with far-search tags.
jobject MakeJObject(JNIEnv *env, const far_search::Det &d) {
  static jmethodID constructor =
      env->GetMethodID(detectionCls, "<init>", "(Ljava/lang/String;IIF[DDD[D)V");
  if (!constructor) return nullptr;
  wpi::java::JLocal<jstring> fam{env, wpi::java::MakeJString(env, d.family->name)};
  wpi::java::JLocal<jdoubleArray> harr{
      env, wpi::java::MakeJDoubleArray(env, {reinterpret_cast<const jdouble *>(d.H.data()), d.H.size()})};
  wpi::java::JLocal<jdoubleArray> carr{
      env, wpi::java::MakeJDoubleArray(env, {reinterpret_cast<const jdouble *>(d.p.data()), d.p.size()})};
  return env->NewObject(detectionCls, constructor, fam.obj(), static_cast<jint>(d.id),
                        static_cast<jint>(d.hamming), static_cast<jfloat>(d.margin), harr.obj(),
                        static_cast<jdouble>(d.c[0]), static_cast<jdouble>(d.c[1]), carr.obj());
}

// A line every 10 s while the far search has done anything since the last one.
void ReportFarSearch() {
  static std::mutex mu;
  static auto next = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  static far_search::Counters last{};
  std::lock_guard<std::mutex> lock(mu);
  const auto now = std::chrono::steady_clock::now();
  if (now < next) return;
  next = now + std::chrono::seconds(10);
  const auto c = far_search::GetCounters();
  const long sweeps = c.sweeps - last.sweeps, crops = c.crops - last.crops, tags = c.far_tags - last.far_tags;
  const long starved = c.starved_ms - last.starved_ms;
  if (sweeps || crops || tags || starved) {
    std::cout << "971 far search 10 s: starved " << starved / 1000.0 << " s, " << sweeps
              << " full-size searches (" << (sweeps ? (c.sweep_ms - last.sweep_ms) / sweeps : 0)
              << " ms each), " << crops << " crops ("
              << (crops ? (c.crop_ms - last.crop_ms) / crops : 0) << " ms each), " << tags
              << " far tags" << std::endl;
  }
  last = c;
}

jobjectArray MakeJObjectArray(JNIEnv *env, const std::vector<far_search::Det> &dets) {
  jobjectArray jarr = env->NewObjectArray(static_cast<jsize>(dets.size()), detectionCls, nullptr);
  if (!jarr) return nullptr;
  for (size_t i = 0; i < dets.size(); ++i) {
    wpi::java::JLocal<jobject> elem{env, MakeJObject(env, dets[i])};
    env->SetObjectArrayElement(jarr, static_cast<jsize>(i), elem.obj());
  }
  return jarr;
}

// The detection mask (setMask) applied to far-search tags by their centre: the far search's own
// detectors have no mask.
bool MaskKeeps(const DetectorSlot &s, int width, int height, double cx, double cy) {
  const size_t n = s.mask_rects.size() / 4;
  if (s.mask_mode == 0 || n == 0) return true;
  bool inside = false;
  for (size_t i = 0; i < n && !inside; ++i) {
    const double x = s.mask_rects[4 * i] * width, y = s.mask_rects[4 * i + 1] * height;
    const double w = s.mask_rects[4 * i + 2] * width, h = s.mask_rects[4 * i + 3] * height;
    inside = cx >= x && cx < x + w && cy >= y && cy < y + h;
  }
  return s.mask_mode == 1 ? !inside : inside;
}

jobjectArray MakeJObjectArray(JNIEnv *env, const zarray_t *detections) {
  int n = detections ? zarray_size(detections) : 0;
  jobjectArray jarr = env->NewObjectArray(n, detectionCls, nullptr);
  if (!jarr) return nullptr;
  for (int i = 0; i < n; ++i) {
    apriltag_detection_t *det;
    zarray_get(detections, i, &det);
    wpi::java::JLocal<jobject> elem{env, MakeJObject(env, det)};
    env->SetObjectArrayElement(jarr, i, elem.obj());
  }
  return jarr;
}

void RecordStats(Stats &st, jlong handle, const cv::Mat &img, const zarray_t *detections,
                 bool error, std::chrono::steady_clock::time_point t0,
                 std::chrono::steady_clock::time_point t1,
                 std::chrono::steady_clock::time_point t2) {
  using ms = std::chrono::duration<double, std::milli>;
  if (st.frames == 0) st.start = t0;
  double d = ms(t1 - t0).count();
  st.frames++;
  st.errors += error;
  st.detect_ms += d;
  st.jni_ms += ms(t2 - t1).count();
  if (d > st.max_ms) st.max_ms = d;
  int n = detections ? zarray_size(detections) : 0;
  st.tags += n;
  for (int i = 0; i < n; ++i) {
    apriltag_detection_t *det;
    zarray_get(detections, i, &det);
    st.margin += det->decision_margin;
    if (det->decision_margin < st.min_margin) st.min_margin = det->decision_margin;
  }
  double window = std::chrono::duration<double>(t2 - st.start).count();
  if (window >= 1.0) {
    std::cout << "971 stats h" << handle << " " << img.cols << "x" << img.rows << ": "
              << st.frames / window << " calls/s, detect avg " << st.detect_ms / st.frames
              << " ms max " << st.max_ms << " ms, jni " << st.jni_ms / st.frames
              << " ms, tags/frame " << double(st.tags) / st.frames;
    if (st.tags) {
      std::cout << ", margin avg " << st.margin / st.tags << " min " << st.min_margin;
    }
    if (st.errors) std::cout << ", errors " << st.errors;
    if (st.lock_wait_ms > 0) std::cout << ", gpu lock wait " << st.lock_wait_ms / st.frames << " ms";
    if (st.gpu_input) std::cout << ", gpu input " << 100 * st.gpu_input / st.frames << "%";
    if (st.ages) std::cout << ", frame age at result " << st.age_ms / st.ages << " ms";
    std::cout << " [bos]" << std::endl;
    st = Stats{};
  }
}

// ---- MJPEG -> gray (decodeMjpegGray) and -> BGR (decodeMjpegBgr) ----------------------------------
// libjpeg-turbo by default. SPECTRUM_JPEG_DECODER=nvjpg uses the Jetson's NVJPG hardware engine
// instead (libspectrumnvjpg.so, nvjpg_decoder.h): 2.5 ms and 0.7 ms of CPU a frame, against
// 2.9 ms and 2.9 ms. /tmp/spectrum-jpeg-decoder ("nvjpg" or "turbo", re-read every 2 s)
// overrides it without a restart, for A/B tests.
//   - A frame the hardware can't decode is decoded by libjpeg-turbo instead.
//   - Every kCheckEvery hardware frames per camera, a low-priority thread decodes the same JPEG
//     with libjpeg-turbo. Any difference turns the hardware decoder off until PhotonVision
//     restarts: the obvious way of calling libnvjpeg returned stale frames with no error
//     (docs/VISION-RESEARCH.md), so the output is checked, not trusted.

struct JpegError {
  jpeg_error_mgr mgr;
  std::jmp_buf jump;
  int warnings;
};
void JpegErrorExit(j_common_ptr c) {  // libjpeg's default calls exit(): never in a JVM
  std::longjmp(reinterpret_cast<JpegError *>(c->err)->jump, 1);
}
void JpegCountWarnings(j_common_ptr c, int level) {
  if (level < 0) reinterpret_cast<JpegError *>(c->err)->warnings++;  // corrupt data
}

// libjpeg-turbo to gray (channels 1) or BGR (channels 3). 0 ok, -2 bad/unsupported JPEG,
// -3 not width x height. *warnings (if given) counts libjpeg's corrupt-data warnings, e.g. a
// truncated frame. BGR uses libjpeg's defaults, which is what cscore's cv::imdecode gives.
int TurboDecode(const uint8_t *jpeg, size_t size, uint8_t *out, int width, int height,
                size_t stride, int channels, int *warnings = nullptr) {
  jpeg_decompress_struct c;
  JpegError err;
  c.err = jpeg_std_error(&err.mgr);
  err.mgr.error_exit = JpegErrorExit;
  err.mgr.emit_message = JpegCountWarnings;
  err.warnings = 0;
  if (setjmp(err.jump)) {
    jpeg_destroy_decompress(&c);
    return -2;
  }
  jpeg_create_decompress(&c);
  jpeg_mem_src(&c, const_cast<unsigned char *>(jpeg), size);
  if (jpeg_read_header(&c, TRUE) != JPEG_HEADER_OK) {
    jpeg_destroy_decompress(&c);
    return -2;
  }
  c.out_color_space = channels == 1 ? JCS_GRAYSCALE : JCS_EXT_BGR;
  c.dct_method = JDCT_ISLOW;  // accurate: tag corners depend on clean edges (and the default)
  jpeg_start_decompress(&c);
  if (static_cast<int>(c.output_width) != width || static_cast<int>(c.output_height) != height ||
      c.output_components != channels) {
    jpeg_abort_decompress(&c);
    jpeg_destroy_decompress(&c);
    return -3;
  }
  while (c.output_scanline < c.output_height) {
    JSAMPROW row = out + static_cast<size_t>(c.output_scanline) * stride;
    jpeg_read_scanlines(&c, &row, 1);
  }
  jpeg_finish_decompress(&c);
  jpeg_destroy_decompress(&c);
  if (warnings) *warnings = err.warnings;
  return 0;
}

int TurboDecodeGray(const uint8_t *jpeg, size_t size, uint8_t *gray, int width, int height,
                    size_t stride) {
  return TurboDecode(jpeg, size, gray, width, height, stride, 1);
}

struct Nvjpg {
  decltype(&snj_create) create;
  decltype(&snj_create_error) create_error;
  decltype(&snj_destroy) destroy;
  decltype(&snj_decode_gray) decode;
  decltype(&snj_decode_bgr) decode_bgr;  // null in a library older than the colour path
  decltype(&snj_decode_gray_dev) decode_gray_dev;  // null in a library older than GPU input
  decltype(&snj_error) error;
};

// libspectrumnvjpg.so from this library's own directory (so an uninstalled build tests its own
// copy), else from the search path. RTLD_DEEPBIND: its jpeg_* calls must bind to libnvjpeg, not
// to the libjpeg-turbo this library links.
const Nvjpg *LoadNvjpg() {
  static const Nvjpg *api = []() -> const Nvjpg * {
    std::string path = "libspectrumnvjpg.so";
    Dl_info self;
    if (dladdr(reinterpret_cast<void *>(&TurboDecodeGray), &self) && self.dli_fname) {
      std::string dir = self.dli_fname;
      dir.erase(dir.rfind('/') + 1);
      if (access((dir + path).c_str(), R_OK) == 0) path = dir + path;
    }
    void *h = dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL | RTLD_DEEPBIND);
    if (!h) {
      std::cout << "971 jpeg: can't load " << path << " (" << dlerror()
                << "); using libjpeg-turbo" << std::endl;
      return nullptr;
    }
    static Nvjpg a;
    a.create = reinterpret_cast<decltype(a.create)>(dlsym(h, "snj_create"));
    a.create_error = reinterpret_cast<decltype(a.create_error)>(dlsym(h, "snj_create_error"));
    a.destroy = reinterpret_cast<decltype(a.destroy)>(dlsym(h, "snj_destroy"));
    a.decode = reinterpret_cast<decltype(a.decode)>(dlsym(h, "snj_decode_gray"));
    a.decode_bgr = reinterpret_cast<decltype(a.decode_bgr)>(dlsym(h, "snj_decode_bgr"));
    a.decode_gray_dev =
        reinterpret_cast<decltype(a.decode_gray_dev)>(dlsym(h, "snj_decode_gray_dev"));
    a.error = reinterpret_cast<decltype(a.error)>(dlsym(h, "snj_error"));
    if (!a.create || !a.create_error || !a.destroy || !a.decode || !a.error) {
      std::cout << "971 jpeg: " << path << " is missing functions; using libjpeg-turbo"
                << std::endl;
      return nullptr;
    }
    std::cout << "971 jpeg: loaded " << path << (a.decode_bgr ? " (gray and colour)" : " (gray only)")
              << std::endl;
    return &a;
  }();
  return api;
}

enum class JpegDecoder { kTurbo, kNvjpg };

// Set when a check finds the hardware's output differs, or on a CUDA failure: from then on
// libjpeg-turbo decodes everything until PhotonVision restarts.
std::atomic<bool> nvjpg_off{false};

// "nvjpg:N": only the first N cameras (camera threads, in the order they first decoded) use the
// hardware decoder for gray frames, the rest libjpeg-turbo. 4 cameras share the Jetson's 2 NVJPG
// engines, which made a hardware decode ~2.8 ms against ~2.4 ms on the CPU (2026-09-29); moving
// some cameras to the CPU (which has idle cores) shortens the hardware's queue. -1: no limit.
std::atomic<int> nvjpg_camera_limit{-1};

JpegDecoder WantedJpegDecoder() {
  static std::mutex mu;
  static std::chrono::steady_clock::time_point next{};
  static JpegDecoder mode = JpegDecoder::kTurbo;
  static std::string logged;
  std::lock_guard<std::mutex> lock(mu);
  const auto now = std::chrono::steady_clock::now();
  if (now >= next) {
    next = now + std::chrono::seconds(2);
    std::string v = "turbo";
    if (const char *e = std::getenv("SPECTRUM_JPEG_DECODER")) v = e;
    if (FILE *f = std::fopen("/tmp/spectrum-jpeg-decoder", "r")) {
      char buf[16] = {0};
      if (std::fgets(buf, sizeof(buf), f)) v = std::string(buf).substr(0, std::strcspn(buf, " \r\n"));
      std::fclose(f);
    }
    int limit = -1;
    if (v.rfind("nvjpg:", 0) == 0) {
      limit = std::atoi(v.c_str() + 6);
      if (limit < 0) limit = -1;
    }
    nvjpg_camera_limit = limit;
    mode = v.rfind("nvjpg", 0) == 0 ? JpegDecoder::kNvjpg : JpegDecoder::kTurbo;
    if (v != logged) {
      logged = v;
      std::cout << "971 jpeg decoder: "
                << (mode == JpegDecoder::kNvjpg
                        ? (limit >= 0 ? "nvjpg (hardware) for the first " + std::to_string(limit) +
                                            " cameras, libjpeg-turbo for the rest"
                                      : std::string("nvjpg (hardware)"))
                        : std::string("libjpeg-turbo"))
                << (nvjpg_off ? " requested, but the hardware decoder is off (see above)" : "")
                << std::endl;
    }
  }
  return nvjpg_off ? JpegDecoder::kTurbo : mode;
}

// This camera thread's place in the order cameras first decoded, for nvjpg:N.
bool HardwareForThisCamera() {
  static std::atomic<int> next_slot{0};
  thread_local int slot = next_slot++;
  const int limit = nvjpg_camera_limit;
  return limit < 0 || slot < limit;
}

std::atomic<long> checks_ok{0}, checks_differ{0}, checks_skipped{0};

// Re-decodes copies of hardware-decoded frames with libjpeg-turbo on its own low-priority
// thread, and compares. One job at a time; a frame offered while it's busy isn't checked.
// (Plain buffers, not cv::Mat: this library uses only OpenCV's header-inline parts, because
// PhotonVision brings its own OpenCV build.)
class JpegChecker {
 public:
  // channels: 1 = gray, 3 = BGR.
  void Submit(const uint8_t *jpeg, size_t size, const uint8_t *image, int width, int height,
              size_t stride, int channels) {
    std::lock_guard<std::mutex> lock(mu_);
    if (busy_) return;
    busy_ = true;
    jpeg_.assign(jpeg, jpeg + size);
    width_ = width;
    height_ = height;
    channels_ = channels;
    const size_t row = static_cast<size_t>(width) * channels;
    image_.resize(row * height);
    for (int y = 0; y < height; ++y) std::memcpy(&image_[y * row], image + y * stride, row);
    if (!thread_.joinable()) thread_ = std::thread([this] { Run(); });
    cv_.notify_one();
  }

 private:
  void Run() {
    setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), 10);
    std::vector<uint8_t> ref;
    for (;;) {
      {
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait(lock, [this] { return busy_; });
      }
      ref.resize(image_.size());
      int warnings = 0;
      const int rc = TurboDecode(jpeg_.data(), jpeg_.size(), ref.data(), width_, height_,
                                 static_cast<size_t>(width_) * channels_, channels_, &warnings);
      if (rc != 0 || warnings) {
        checks_skipped++;  // a corrupt frame: libjpeg-turbo and NVJPG may fill the gap differently
      } else {
        long differ = 0;
        int max_diff = 0;
        for (size_t i = 0; i < ref.size(); ++i) {
          const int d = std::abs(ref[i] - image_[i]);
          differ += d != 0;
          max_diff = std::max(max_diff, d);
        }
        if (differ == 0) {
          checks_ok++;
        } else {
          checks_differ++;
          nvjpg_off = true;
          std::cout << "971 jpeg: HARDWARE DECODE DIFFERS from libjpeg-turbo ("
                    << (channels_ == 1 ? "gray" : "colour") << ", " << differ << " of "
                    << ref.size() << " values, max difference " << max_diff
                    << "); libjpeg-turbo decodes everything until PhotonVision restarts"
                    << std::endl;
        }
      }
      std::lock_guard<std::mutex> lock(mu_);
      busy_ = false;
    }
  }

  std::mutex mu_;
  std::condition_variable cv_;
  bool busy_ = false;
  std::thread thread_;
  std::vector<uint8_t> jpeg_, image_;
  int width_ = 0, height_ = 0, channels_ = 1;
};

JpegChecker &Checker() {
  static JpegChecker *c = new JpegChecker;  // never destroyed: its thread runs until exit
  return *c;
}

// ~2 s at 120 fps. The first hardware frame on each camera is checked too.
constexpr long kCheckEvery = 240;

// One hardware decoder per camera thread (a decoder isn't thread-safe; PhotonVision decodes
// each camera's frames on that camera's own thread).
struct NvjpgThread {
  SnjDecoder *dec = nullptr;
  bool unavailable = false;       // no decoder
  bool path_unavailable[2] = {};  // [0] gray, [1] colour: this camera's JPEGs can't use it
  int unsupported_in_a_row[2] = {};
  long frames = 0;
  // GPU input: the last gray frame decoded on this thread, also on the GPU (dev, dev_w x dev_h),
  // while dev_valid. fp: 64 of its pixels, to recognise it in processimage (PhotonVision hands
  // the detector a copy, and a rotation would change the pixels in place).
  uint8_t *dev = nullptr;
  int dev_w = 0, dev_h = 0;
  bool dev_valid = false;
  uint8_t fp[64];
  ~NvjpgThread() {
    if (dec) LoadNvjpg()->destroy(dec);
    if (dev) cudaFree(dev);
  }
};
thread_local NvjpgThread nvjpg_thread;

// GPU input: the hardware decoder leaves each gray frame on the GPU too, and processimage hands
// that copy to Detect(), which otherwise copies the frame it was given back up to the GPU (48 us
// of GPU and a ~270 us call a frame at 1280x800, nsys 2026-09-29). Only when processimage gets
// the frame that was just decoded on the same thread (same size and the same 64 sampled pixels;
// a full comparison every kCheckEvery frames). "0" in /tmp/spectrum-971-gpu-input
// turns it off (re-read every 2 s).
std::atomic<bool> gpu_input_off{false};  // a check found the GPU copy differing: off until restart
// The pixels FingerprintGpuInput samples: spread over the frame (Knuth's multiplicative hash).
size_t FingerprintIndex(int i, size_t n) { return (static_cast<size_t>(i) * 2654435761u) % n; }
void FingerprintGpuInput(const uint8_t *img, size_t n, uint8_t *fp) {
  for (int i = 0; i < 64; ++i) fp[i] = img[FingerprintIndex(i, n)];
}
bool MatchesGpuInput(const uint8_t *img, size_t n, const uint8_t *fp) {
  for (int i = 0; i < 64; ++i) {
    if (fp[i] != img[FingerprintIndex(i, n)]) return false;
  }
  return true;
}

bool GpuInputOn() {
  if (gpu_input_off) return false;
  static std::mutex mu;
  static std::chrono::steady_clock::time_point next{};
  static bool on = true;
  std::lock_guard<std::mutex> lock(mu);
  const auto now = std::chrono::steady_clock::now();
  if (now >= next) {
    next = now + std::chrono::seconds(2);
    int v = 1;
    if (FILE *f = std::fopen("/tmp/spectrum-971-gpu-input", "r")) {
      if (std::fscanf(f, "%d", &v) != 1) v = 1;
      std::fclose(f);
    }
    on = v != 0;
  }
  return on;
}

std::atomic<int> nvjpg_errors_logged{0};

// Test hook for the safety net: while /tmp/spectrum-jpeg-fault exists (checked every 30 frames),
// every hardware-decoded frame gets one pixel changed, so the next check must find it and turn
// the hardware decoder off.
bool JpegFaultActive() {
  static std::atomic<int> calls{0};
  static std::atomic<bool> active{false};
  if (calls++ % 30 == 0) active = access("/tmp/spectrum-jpeg-fault", F_OK) == 0;
  return active;
}

// Decodes into mat (CV_8UC1 gray, or CV_8UC3 BGR when bgr). SNJ_OK, or why libjpeg-turbo has to
// decode this frame instead.
int NvjpgDecode(const uint8_t *jpeg, size_t size, cv::Mat &mat, bool bgr) {
  NvjpgThread &t = nvjpg_thread;
  if (t.unavailable || t.path_unavailable[bgr]) return SNJ_UNSUPPORTED;
  const Nvjpg *api = LoadNvjpg();
  if (!api || (bgr && !api->decode_bgr)) {
    if (!api) t.unavailable = true;
    t.path_unavailable[bgr] = true;
    return SNJ_UNSUPPORTED;
  }
  if (!t.dec && !(t.dec = api->create())) {
    std::cout << "971 jpeg: no hardware decoder (" << api->create_error()
              << "); using libjpeg-turbo on this camera" << std::endl;
    t.unavailable = true;
    return SNJ_UNSUPPORTED;
  }
  t.dev_valid = false;
  bool to_dev = !bgr && api->decode_gray_dev && GpuInputOn();
  if (to_dev && (t.dev_w != mat.cols || t.dev_h != mat.rows)) {
    if (t.dev) cudaFree(t.dev);
    t.dev = nullptr;
    t.dev_w = t.dev_h = 0;
    if (cudaMalloc(reinterpret_cast<void **>(&t.dev), static_cast<size_t>(mat.cols) * mat.rows) ==
        cudaSuccess) {
      t.dev_w = mat.cols;
      t.dev_h = mat.rows;
    } else {
      cudaGetLastError();
      t.dev = nullptr;
    }
  }
  to_dev = to_dev && t.dev;
  const int rc = to_dev ? api->decode_gray_dev(t.dec, jpeg, size, mat.data, mat.cols, mat.rows,
                                               mat.step, t.dev)
                        : (bgr ? api->decode_bgr : api->decode)(t.dec, jpeg, size, mat.data,
                                                                mat.cols, mat.rows, mat.step);
  if (rc == SNJ_OK) {
    t.unsupported_in_a_row[bgr] = 0;
    if (to_dev) {
      t.dev_valid = true;
      FingerprintGpuInput(mat.data, static_cast<size_t>(mat.cols) * mat.rows, t.fp);
    }
    if (JpegFaultActive()) {
      mat.data[0] ^= 0x80;
      t.dev_valid = false;  // the host copy no longer matches
    }
    if (t.frames++ % kCheckEvery == 0) {
      Checker().Submit(jpeg, size, mat.data, mat.cols, mat.rows, mat.step, bgr ? 3 : 1);
    }
    return rc;
  }
  if (nvjpg_errors_logged++ < 20) {
    std::cout << "971 jpeg: hardware decode failed (" << rc << ": " << api->error(t.dec)
              << "); libjpeg-turbo decodes this frame" << std::endl;
  }
  if (rc == SNJ_UNSUPPORTED && ++t.unsupported_in_a_row[bgr] >= 30) {
    std::cout << "971 jpeg: this camera's JPEGs can't use the hardware " << (bgr ? "colour" : "gray")
              << " decoder (" << api->error(t.dec) << "); using libjpeg-turbo on this camera"
              << std::endl;
    t.path_unavailable[bgr] = true;
  }
  if (rc == SNJ_CUDA && !nvjpg_off.exchange(true)) {
    std::cout << "971 jpeg: CUDA failed in the hardware decoder; libjpeg-turbo decodes "
                 "everything until PhotonVision restarts"
              << std::endl;
  }
  return rc;
}

// A "971 jpeg" line every 10 s (not "971 stats", which health-check.sh parses per detector).
// Colour frames are also counted on their own, in a clause at the end of the line.
std::atomic<int> last_timestamp_src{-1};  // WPI_TimestampSource of the last gray frame
// The capture timestamp (wpi::Now microseconds) of the last gray frame decoded on this thread;
// processimage reports how old that frame is when its detection ends ("frame age at result").
thread_local uint64_t last_capture_us = 0;

// age_ms: how old the frame was when its decode started (from cscore's capture timestamp, which
// the camera driver takes when the frame's first USB packet arrives), or < 0 if unknown.
void CountJpeg(JpegDecoder used, int fallback, std::chrono::steady_clock::time_point t0,
               bool colour = false, double age_ms = -1, size_t jpeg_bytes = 0) {
  static std::mutex mu;
  static std::chrono::steady_clock::time_point start = t0;
  static long frames[2] = {0, 0}, colour_frames[2] = {0, 0}, fallbacks[5] = {0, 0, 0, 0, 0};
  static double ms[2] = {0, 0}, colour_ms[2] = {0, 0};
  static double age_sum = 0, age_max = 0, jpeg_kb = 0;
  static long ages = 0, jpegs = 0;
  const auto t1 = std::chrono::steady_clock::now();
  std::lock_guard<std::mutex> lock(mu);
  const int i = used == JpegDecoder::kNvjpg ? 1 : 0;
  const double this_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
  frames[i]++;
  ms[i] += this_ms;
  if (colour) {
    colour_frames[i]++;
    colour_ms[i] += this_ms;
  }
  if (fallback < 0 && fallback >= -4) fallbacks[-fallback]++;
  if (jpeg_bytes) {
    jpeg_kb += jpeg_bytes / 1024.0;
    jpegs++;
  }
  if (age_ms >= 0 && age_ms < 1000) {
    age_sum += age_ms;
    age_max = std::max(age_max, age_ms);
    ages++;
  }
  const double window = std::chrono::duration<double>(t1 - start).count();
  if (window < 10) return;
  std::cout << "971 jpeg " << static_cast<int>(window + 0.5) << " s: nvjpg "
            << frames[1] / window << " frames/s";
  if (frames[1]) std::cout << " (" << ms[1] / frames[1] << " ms)";
  std::cout << ", libjpeg-turbo " << frames[0] / window << " frames/s";
  if (frames[0]) std::cout << " (" << ms[0] / frames[0] << " ms)";
  if (long f = fallbacks[1] + fallbacks[3] + fallbacks[4]) {
    std::cout << "; " << f << " fell back (bad JPEG " << fallbacks[1] << ", unsupported "
              << fallbacks[3] << ", CUDA " << fallbacks[4] << ")";
  }
  std::cout << "; checks since start " << checks_ok << " ok, " << checks_differ << " differ";
  if (checks_skipped) std::cout << ", " << checks_skipped << " skipped (corrupt frame)";
  if (nvjpg_off) std::cout << "; hardware decoder OFF";
  if (ages) {
    std::cout << "; frame age at decode avg " << age_sum / ages << " ms max " << age_max << " ms";
  }
  if (jpegs) std::cout << "; JPEG avg " << jpeg_kb / jpegs << " KB";
  if (static int src = -1; src != last_timestamp_src) {
    src = last_timestamp_src;
    std::cout << "; timestamp source " << src;
  }
  if (colour_frames[0] + colour_frames[1]) {
    std::cout << "; colour: nvjpg " << colour_frames[1] / window << " frames/s";
    if (colour_frames[1]) std::cout << " (" << colour_ms[1] / colour_frames[1] << " ms)";
    std::cout << ", libjpeg-turbo " << colour_frames[0] / window << " frames/s";
    if (colour_frames[0]) std::cout << " (" << colour_ms[0] / colour_frames[0] << " ms)";
  }
  std::cout << std::endl;
  start = t1;
  age_sum = age_max = jpeg_kb = 0;
  ages = jpegs = 0;
  frames[0] = frames[1] = colour_frames[0] = colour_frames[1] = 0;
  ms[0] = ms[1] = colour_ms[0] = colour_ms[1] = 0;
  for (auto &f : fallbacks) f = 0;
}

}  // namespace

extern "C" {

__attribute__((visibility("default"))) void spectrum_cuda_lock_shared() {
  cuda_capture_lock.lock_shared();
}
__attribute__((visibility("default"))) void spectrum_cuda_unlock_shared() {
  cuda_capture_lock.unlock_shared();
}

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM *vm, void *) {
  JNIEnv *env;
  if (vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK) return JNI_ERR;
  for (auto &c : classes) {
    *c.cls = wpi::java::JClass(env, c.name);
    if (!*c.cls) {
      std::cout << "971 library could not find class " << c.name << std::endl;
      return JNI_ERR;
    }
  }
  std::string sync;
  const cudaError_t sync_err = cudaSetDeviceFlags(CudaScheduleFlag(&sync));
  // CUDA reads CUDA_DEVICE_MAX_CONNECTIONS (default 8) from the process environment, which
  // 08-select-detector.sh sets; only reported here.
  const char *connections = std::getenv("CUDA_DEVICE_MAX_CONNECTIONS");
  std::cout << "971 library loaded (frc971/bos detector, min_white_black_diff "
            << MinWhiteBlackDiff() << ", max_line_fit_mse " << MaxLineFitMse() << ", threads "
            << DetectorThreads() << ", CUDA wait " << sync
            << (sync_err == cudaSuccess ? "" : std::string(" FAILED: ") + cudaGetErrorString(sync_err))
            << ", GPU connections " << (connections ? connections : "8") << ")" << std::endl;
  return JNI_VERSION_1_6;
}

// SpectrumJetson: decode a camera's MJPEG frame straight to 8-bit gray, into a Mat Java already
// allocated (PhotonVision's OpenCV owns the memory; this only writes the pixels).
//   int decodeMjpegGray(long rawFramePtr, long cvMatPtr)
//     0 ok, -1 not an MJPEG frame, -2 bad/unsupported JPEG, -3 Mat isn't WxH 8-bit mono
// Why: cscore's own gray path decodes every JPEG to full-colour BGR and then converts it
// (Frame::ConvertImpl), ~8.9 ms a frame on the Orin Nano; this is ~2.6 ms (libjpeg-turbo,
// grayscale output: only the Y component is inverse-DCT'd, no colour conversion), or 0.7 ms of
// CPU on the NVJPG hardware engine (see "MJPEG -> gray" above).
JNIEXPORT jint JNICALL Java_org_photonvision_jni_GpuDetectorJNI_decodeMjpegGray(
    JNIEnv *, jclass, jlong raw_ptr, jlong mat_ptr) {
  auto *frame = reinterpret_cast<WPI_RawFrame *>(raw_ptr);
  auto *mat = reinterpret_cast<cv::Mat *>(mat_ptr);
  if (!frame || !mat) return -2;
  if (frame->pixelFormat != WPI_PIXFMT_MJPEG || !frame->data || frame->size < 4) return -1;
  if (mat->type() != CV_8UC1 || !mat->isContinuous()) return -3;
  const auto *data = reinterpret_cast<const uint8_t *>(frame->data);
  const size_t size = static_cast<size_t>(frame->size);

  const auto t0 = std::chrono::steady_clock::now();
  const double age_ms =
      frame->timestamp ? (static_cast<double>(wpi::Now()) - static_cast<double>(frame->timestamp)) / 1000
                       : -1;
  last_timestamp_src = frame->timestampSrc;
  last_capture_us = frame->timestamp;
  // Latency probe (tests only): while /tmp/spectrum-971-kmsg exists (checked every 60 frames),
  // log each decode start to the kernel log, next to uvcvideo's "Frame complete" trace lines.
  {
    static std::atomic<int> calls{0};
    static std::atomic<bool> on{false};
    if (calls++ % 60 == 0) on = access("/tmp/spectrum-971-kmsg", F_OK) == 0;
    if (on) {
      if (FILE *k = std::fopen("/dev/kmsg", "w")) {
        std::fprintf(k, "971 decode start tid %ld age %.3f\n", syscall(SYS_gettid), age_ms);
        std::fclose(k);
      }
    }
  }
  int fallback = SNJ_OK;
  nvjpg_thread.dev_valid = false;  // until a hardware decode puts this frame on the GPU
  if (WantedJpegDecoder() == JpegDecoder::kNvjpg && HardwareForThisCamera()) {
    {
      std::shared_lock<CudaCaptureLock> cuda(cuda_capture_lock);
      fallback = NvjpgDecode(data, size, *mat, /*bgr=*/false);
    }
    if (fallback == SNJ_OK) {
      CountJpeg(JpegDecoder::kNvjpg, SNJ_OK, t0, false, age_ms, size);
      return 0;
    }
    if (fallback == SNJ_WRONG_SIZE) return -3;
  }
  const int rc = TurboDecodeGray(data, size, mat->data, mat->cols, mat->rows, mat->step);
  if (rc == 0) CountJpeg(JpegDecoder::kTurbo, fallback, t0, false, age_ms, size);
  return rc;
}

// SpectrumJetson: the same for colour cameras (game pieces, driver mode, calibration): the
// camera's MJPEG frame to 8-bit BGR, into a WxH CV_8UC3 Mat Java allocated.
//   boolean hardwareJpegDecode()   true when PhotonVision should use decodeMjpegBgr instead of
//                                  cscore's own BGR decode (the hardware decoder is on and loaded)
//   int decodeMjpegBgr(long rawFramePtr, long cvMatPtr)
//     0 ok, -1 not an MJPEG frame, -2 bad/unsupported JPEG, -3 Mat isn't WxH 8-bit BGR
// NVJPG plus a CUDA colour conversion (nvjpg_bgr.cu) takes 3.3-4 ms and ~1.6 ms of CPU a
// 1280x800 frame, against ~5.7 ms of CPU for libjpeg-turbo, with identical pixels. A frame the
// hardware can't decode (e.g. 4:2:0 chroma) is decoded by libjpeg-turbo, like cscore would.
JNIEXPORT jboolean JNICALL Java_org_photonvision_jni_GpuDetectorJNI_hardwareJpegDecode(JNIEnv *,
                                                                                       jclass) {
  if (WantedJpegDecoder() != JpegDecoder::kNvjpg) return JNI_FALSE;
  const Nvjpg *api = LoadNvjpg();
  return api && api->decode_bgr ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jint JNICALL Java_org_photonvision_jni_GpuDetectorJNI_decodeMjpegBgr(
    JNIEnv *, jclass, jlong raw_ptr, jlong mat_ptr) {
  auto *frame = reinterpret_cast<WPI_RawFrame *>(raw_ptr);
  auto *mat = reinterpret_cast<cv::Mat *>(mat_ptr);
  if (!frame || !mat) return -2;
  if (frame->pixelFormat != WPI_PIXFMT_MJPEG || !frame->data || frame->size < 4) return -1;
  if (mat->type() != CV_8UC3 || !mat->isContinuous()) return -3;
  const auto *data = reinterpret_cast<const uint8_t *>(frame->data);
  const size_t size = static_cast<size_t>(frame->size);

  const auto t0 = std::chrono::steady_clock::now();
  int fallback = SNJ_OK;
  if (WantedJpegDecoder() == JpegDecoder::kNvjpg) {
    {
      std::shared_lock<CudaCaptureLock> cuda(cuda_capture_lock);
      fallback = NvjpgDecode(data, size, *mat, /*bgr=*/true);
    }
    if (fallback == SNJ_OK) {
      CountJpeg(JpegDecoder::kNvjpg, SNJ_OK, t0, /*colour=*/true);
      return 0;
    }
    if (fallback == SNJ_WRONG_SIZE) return -3;
  }
  const int rc = TurboDecode(data, size, mat->data, mat->cols, mat->rows, mat->step, 3);
  if (rc == 0) CountJpeg(JpegDecoder::kTurbo, fallback, t0, /*colour=*/true);
  return rc;
}

JNIEXPORT void JNICALL JNI_OnUnload(JavaVM *vm, void *) {
  JNIEnv *env;
  if (vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) != JNI_OK) return;
  for (auto &c : classes) c.cls->free(env);
}

JNIEXPORT jlong JNICALL Java_org_photonvision_jni_GpuDetectorJNI_createGpuDetector(
    JNIEnv *, jclass, jint width, jint height) {
  std::lock_guard<std::mutex> alloc_lock(alloc_mu);
  int h = -1;
  for (int i = 0; i < kMaxDetectors; ++i) {
    if (!slots[i].in_use) {
      h = i;
      break;
    }
  }
  if (h < 0) {
    std::cout << "creategpudetector: all " << kMaxDetectors << " slots in use" << std::endl;
    return -1;
  }
  DetectorSlot &s = slots[h];
  std::lock_guard<std::mutex> lock(s.mu);
  std::shared_lock<CudaCaptureLock> cuda(cuda_capture_lock);
  s.camera_matrix = DefaultCameraMatrix();
  s.dist_coeffs = DefaultDistCoeffs();
  s.family = tag36h11_create();
  s.td = MakeTagDetector(s.family);
  s.consecutive_failures = 0;
  s.mask_mode = 0;  // a reused slot must not keep the previous detector's mask
  s.mask_rects.clear();
  s.mask_dirty = true;
  s.last_graph_record = {};
  // If the GPU build fails, keep the slot: processimage retries the build each frame.
  if (!Rebuild(s, width, height)) s.needs_rebuild = true;
  s.stats = Stats{};
  s.in_use = true;
  std::cout << "creategpudetector " << width << "x" << height << " handle " << h << std::endl;
  return h;
}

JNIEXPORT void JNICALL Java_org_photonvision_jni_GpuDetectorJNI_destroyGpuDetector(
    JNIEnv *, jclass, jlong handle) {
  std::lock_guard<std::mutex> alloc_lock(alloc_mu);
  DetectorSlot *s = Slot(handle);
  if (!s) {
    std::cout << "destroygpudetector: bad handle " << handle << std::endl;
    return;
  }
  std::lock_guard<std::mutex> lock(s->mu);
  std::shared_lock<CudaCaptureLock> cuda(cuda_capture_lock);
  far_search::Forget(static_cast<int>(handle));
  delete s->gpu;
  s->gpu = nullptr;
  if (s->td) apriltag_detector_destroy(s->td);
  s->td = nullptr;
  if (s->family) tag36h11_destroy(s->family);
  s->family = nullptr;
  s->in_use = false;
  std::cout << "destroygpudetector handle " << handle << std::endl;
}

namespace {

// Stores new intrinsics; the detector is rebuilt with them on the next frame.
// num_params 5 = k1 k2 p1 p2 k3 (k4..k6 zero); 8 = OpenCV rational model, which is what
// PhotonVision's (mrcal) calibration produces.
void SetParams(jlong handle, double fx, double cx, double fy, double cy, double k1, double k2,
               double p1, double p2, double k3, double k4, double k5, double k6,
               int num_params) {
  DetectorSlot *s = Slot(handle);
  if (!s) {
    std::cout << "setparams: bad handle " << handle << std::endl;
    return;
  }
  std::lock_guard<std::mutex> lock(s->mu);
  s->camera_matrix = frc::apriltag::CameraMatrix{fx, cx, fy, cy};
  s->dist_coeffs = DefaultDistCoeffs();
  s->dist_coeffs.k1 = k1;
  s->dist_coeffs.k2 = k2;
  s->dist_coeffs.p1 = p1;
  s->dist_coeffs.p2 = p2;
  s->dist_coeffs.k3 = k3;
  s->dist_coeffs.k4 = k4;
  s->dist_coeffs.k5 = k5;
  s->dist_coeffs.k6 = k6;
  s->dist_coeffs.num_params = num_params;
  std::cout << "setparams handle " << handle << " (" << num_params << " dist coeffs): fx " << fx
            << " cx " << cx << " fy " << fy << " cy " << cy << " k1 " << k1 << " k2 " << k2
            << " p1 " << p1 << " p2 " << p2 << " k3 " << k3;
  if (num_params == 8) std::cout << " k4 " << k4 << " k5 " << k5 << " k6 " << k6;
  std::cout << std::endl;
  // Takes effect on the next frame (processimage rebuilds at the frame's size).
  s->needs_rebuild = true;
}

}  // namespace

JNIEXPORT void JNICALL Java_org_photonvision_jni_GpuDetectorJNI_setparams(
    JNIEnv *, jclass, jlong handle, jdouble fx, jdouble cx, jdouble fy, jdouble cy, jdouble k1,
    jdouble k2, jdouble p1, jdouble p2, jdouble k3) {
  SetParams(handle, fx, cx, fy, cy, k1, k2, p1, p2, k3, 0, 0, 0, 5);
}

// SpectrumJetson (bos-07): the detection mask. mode 0 off, 1 ignore inside the boxes, 2 search
// only inside them; rects: x, y, w, h fractions of the image, 4 per box. Takes effect next frame.
JNIEXPORT void JNICALL Java_org_photonvision_jni_GpuDetectorJNI_setMask(JNIEnv *env, jclass,
                                                                        jlong handle, jint mode,
                                                                        jdoubleArray rects) {
  DetectorSlot *s = Slot(handle);
  if (!s) {
    std::cout << "setMask: bad handle " << handle << std::endl;
    return;
  }
  std::vector<double> r;
  if (rects) {
    const jsize n = env->GetArrayLength(rects);
    r.resize(static_cast<size_t>(n - n % 4));
    if (!r.empty()) env->GetDoubleArrayRegion(rects, 0, static_cast<jsize>(r.size()), r.data());
  }
  std::lock_guard<std::mutex> lock(s->mu);
  if (mode == s->mask_mode && r == s->mask_rects) return;
  s->mask_mode = mode >= 0 && mode <= 2 ? mode : 0;
  s->mask_rects = std::move(r);
  s->mask_dirty = true;
}

JNIEXPORT void JNICALL Java_org_photonvision_jni_GpuDetectorJNI_setparams8(
    JNIEnv *, jclass, jlong handle, jdouble fx, jdouble cx, jdouble fy, jdouble cy, jdouble k1,
    jdouble k2, jdouble p1, jdouble p2, jdouble k3, jdouble k4, jdouble k5, jdouble k6) {
  SetParams(handle, fx, cx, fy, cy, k1, k2, p1, p2, k3, k4, k5, k6, 8);
}

JNIEXPORT jobjectArray JNICALL Java_org_photonvision_jni_GpuDetectorJNI_processimage(
    JNIEnv *env, jclass, jlong handle, jlong p) {
  if (!p) return nullptr;
  cv::Mat &img = *reinterpret_cast<cv::Mat *>(p);
  if (!img.ptr()) return nullptr;
  if (img.type() != CV_8UC1 || !img.isContinuous()) {
    static bool logged = false;
    if (!logged) {
      logged = true;
      std::cout << "processimage: need a continuous 8-bit mono Mat, got type " << img.type()
                << std::endl;
    }
    return nullptr;
  }
  if (img.cols % 8 != 0 || img.rows % 8 != 0) {
    // threshold.cc CHECKs this; refuse instead of aborting the JVM.
    static bool logged = false;
    if (!logged) {
      logged = true;
      std::cout << "processimage: " << img.cols << "x" << img.rows
                << " is not a multiple of 8; skipping" << std::endl;
    }
    return nullptr;
  }

  DetectorSlot *s = Slot(handle);
  if (!s) {
    std::cout << "processimage: bad handle " << handle << std::endl;
    return nullptr;
  }
  std::lock_guard<std::mutex> lock(s->mu);
  std::shared_lock<CudaCaptureLock> cuda(cuda_capture_lock);

  // Clear any CUDA error left by an earlier unchecked call; CUB (CCCL >= 2.5) otherwise
  // fails later calls with it, and this detector's CHECK_CUDA would abort the process.
  if (cudaError_t stale = cudaGetLastError(); stale != cudaSuccess) {
    static bool logged = false;
    if (!logged) {
      logged = true;
      std::cout << "processimage: cleared stale CUDA error: " << cudaGetErrorString(stale)
                << std::endl;
    }
  }

  if (!s->gpu || s->needs_rebuild || static_cast<size_t>(img.cols) != s->gpu->width() ||
      static_cast<size_t>(img.rows) != s->gpu->height()) {
    if (s->gpu && !s->needs_rebuild) {
      std::cout << "processimage: size changed to " << img.cols << "x" << img.rows
                << ", rebuilding detector" << std::endl;
    }
    if (!Rebuild(*s, img.cols, img.rows)) {
      RecordFailure(*s, handle, "detector rebuild failed");
      return MakeJObjectArray(env, nullptr);
    }
  }
  // The pool is only used inside Detect (tag_detector_->wp and ->nthreads), so it can be
  // swapped between frames.
  if (const int n = DetectorThreads(); n != s->td->nthreads) {
    workerpool_destroy(s->td->wp);
    s->td->nthreads = n;
    s->td->wp = workerpool_create(n);
    std::cout << "971 detector h" << handle << ": " << n << " threads" << std::endl;
  }

  auto t0 = std::chrono::steady_clock::now();
  const zarray_t *detections = nullptr;
  bool failed = false;
  if (int every = FaultEvery(); every > 0) {
    static long frame = 0;
    if (++frame % every == 0) cudaSetDevice(9999);  // leaves "invalid device ordinal"
  } else if (every == kFaultSticky) {
    static bool injected = false;
    if (!injected) {
      injected = true;
      std::cout << "971 TEST: injecting a sticky CUDA fault" << std::endl;
      SpectrumInjectStickyCudaFault();
    }
  }
  std::unique_lock<std::mutex> gpu_lock(gpu_mu, std::defer_lock);
  if (GpuLockOn()) {
    gpu_lock.lock();
    s->stats.lock_wait_ms +=
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  }
  // GPU input (GpuInputOn): the frame is already on the GPU when this is the buffer just decoded.
  NvjpgThread &nt = nvjpg_thread;
  const uint8_t *image_device = nullptr;
  if (nt.dev_valid && nt.dev_w == img.cols && nt.dev_h == img.rows &&
      MatchesGpuInput(img.ptr(), static_cast<size_t>(img.cols) * img.rows, nt.fp)) {
    image_device = nt.dev;
    s->stats.gpu_input++;
    // Safety net, every kCheckEvery such frames per camera (~2 s): the GPU copy must still match
    // the frame Java handed us.
    if (s->gpu_input_frames++ % kCheckEvery == 0) {
      std::vector<uint8_t> back(static_cast<size_t>(img.cols) * img.rows);
      if (cudaMemcpy(back.data(), image_device, back.size(), cudaMemcpyDeviceToHost) !=
              cudaSuccess ||
          std::memcmp(back.data(), img.ptr(), back.size()) != 0) {
        cudaGetLastError();
        if (!gpu_input_off.exchange(true)) {
          std::cout << "971 GPU input: the GPU copy of a frame DIFFERS from the frame given; "
                       "copying frames up again until PhotonVision restarts"
                    << std::endl;
        }
        image_device = nullptr;
      }
    }
  }
  nt.dev_valid = false;  // one frame, one use

  if (s->mask_dirty) {
    ApplyMaskToDetector(*s, img.cols, img.rows);
    std::cout << "971 detector h" << handle << ": mask "
              << (s->mask_mode == 0 ? "off" : s->mask_mode == 1 ? "ignoring" : "searching only")
              << (s->mask_mode ? " " + std::to_string(s->mask_rects.size() / 4) + " box(es)" : "")
              << std::endl;
  }

  // bos-05: record the first-stage graph (once per detector and input buffer) while no other
  // thread uses CUDA (see CudaCaptureLock).
  // Meanwhile (a new mask, say) Detect() runs the steps one by one.
  const auto now_graph = std::chrono::steady_clock::now();
  if (s->gpu && now_graph - s->last_graph_record >= std::chrono::seconds(1) &&
      s->gpu->FirstStageGraphWanted(image_device)) {
    s->last_graph_record = now_graph;
    cuda.unlock();
    {
      std::lock_guard<CudaCaptureLock> exclusive(cuda_capture_lock);
      absl::Status status = s->gpu->RecordFirstStageGraph(image_device);
      std::cout << "971 detector h" << handle << ": first-stage graph "
                << (status.ok() ? "recorded" : std::string(status.message())) << std::endl;
    }
    cuda.lock();
  }
  try {
    absl::Status status = s->gpu->Detect(img.ptr<uint8_t>(), image_device);
    if (status.ok()) {
      detections = s->gpu->Detections();
      s->consecutive_failures = 0;
    } else {
      failed = true;
      RecordFailure(*s, handle, std::string(status.message()).c_str());
    }
  } catch (const std::exception &e) {
    failed = true;
    cudaGetLastError();  // clear a non-sticky error so the rebuild can succeed
    RecordFailure(*s, handle, e.what());
  }
  if (gpu_lock.owns_lock()) gpu_lock.unlock();
  auto t1 = std::chrono::steady_clock::now();

  // SpectrumJetson: the far-tag search (far_search.h): only while no camera has a good view. Its
  // time is kept out of "detect" (the far search keeps its own totals).
  std::vector<far_search::Det> far;
  if (!failed && detections) {
    const int64_t now_us =
        std::chrono::duration_cast<std::chrono::microseconds>(t1.time_since_epoch()).count();
    far = far_search::Process(static_cast<int>(handle), img, far_search::Copy(detections), s->td,
                              s->camera_matrix, s->dist_coeffs, now_us);
    far.erase(std::remove_if(far.begin(), far.end(),
                             [&](const far_search::Det &d) {
                               return !MaskKeeps(*s, img.cols, img.rows, d.c[0], d.c[1]);
                             }),
              far.end());
    ReportFarSearch();
  }
  jobjectArray result;
  if (far.empty()) {
    result = MakeJObjectArray(env, detections);
  } else {
    auto all = far_search::Copy(detections);
    all.insert(all.end(), far.begin(), far.end());
    result = MakeJObjectArray(env, all);
  }
  auto t2 = std::chrono::steady_clock::now();
  if (last_capture_us) {
    const double age = (static_cast<double>(wpi::Now()) - static_cast<double>(last_capture_us)) / 1000;
    if (age >= 0 && age < 1000) {
      s->stats.age_ms += age;
      s->stats.ages++;
    }
    last_capture_us = 0;  // one frame, one reading
  }
  RecordStats(s->stats, handle, img, detections, failed, t0, t1, t2);
  return result;
}

// ---- Far-tag search (far_search.h) --------------------------------------------------------------

// SpectrumJetson: GpuDetectorJNI.setFarSearch(boolean enabled, double sweepsPerSecond), from
// Settings > Robot state (IdleMode in photonvision-55).
JNIEXPORT void JNICALL Java_org_photonvision_jni_GpuDetectorJNI_setFarSearch(JNIEnv *, jclass,
                                                                            jboolean enabled,
                                                                            jdouble sweeps_per_s) {
  far_search::SetConfig({enabled == JNI_TRUE, sweeps_per_s});
}

// double[] farSearchStatus(): {enabled, starved now, full-size searches, crops, far tags returned,
// seconds starved, ms in full-size searches, ms in crops}, all since PhotonVision started.
JNIEXPORT jdoubleArray JNICALL Java_org_photonvision_jni_GpuDetectorJNI_farSearchStatus(JNIEnv *env,
                                                                                       jclass) {
  const auto c = far_search::GetCounters();
  const double v[8] = {far_search::GetConfig().enabled ? 1.0 : 0.0, c.starved ? 1.0 : 0.0,
                       static_cast<double>(c.sweeps), static_cast<double>(c.crops),
                       static_cast<double>(c.far_tags), c.starved_ms / 1000.0, c.sweep_ms, c.crop_ms};
  jdoubleArray arr = env->NewDoubleArray(8);
  if (arr) env->SetDoubleArrayRegion(arr, 0, 8, v);
  return arr;
}

// ---- Status for NetworkTables (JetsonStatusJNI) -------------------------------------------------

// SpectrumJetson: the JPEG decoder's state, for the telemetry PhotonVision publishes under
// /photonvision/jetson (JetsonTelemetry, photonvision-16):
//   long[] nativeJpegStatus()
//     {active decoder (1 nvjpg, 0 libjpeg-turbo), hardware decoder switched off (1/0),
//      checks ok, checks that differed, checks skipped}; counts since PhotonVision started.
JNIEXPORT jlongArray JNICALL Java_org_photonvision_jni_JetsonStatusJNI_nativeJpegStatus(JNIEnv *env,
                                                                                        jclass) {
  const jlong v[5] = {WantedJpegDecoder() == JpegDecoder::kNvjpg ? 1 : 0,
                      nvjpg_off ? 1 : 0,
                      checks_ok.load(),
                      checks_differ.load(),
                      checks_skipped.load()};
  jlongArray arr = env->NewLongArray(5);
  if (arr) env->SetLongArrayRegion(arr, 0, 5, v);
  return arr;
}

}  // extern "C"
