// SpectrumJetson: TensorRT YOLO object detection for PhotonVision on the Jetson.
//
// Java side: org.photonvision.jni.TensorRtJNI (photonvision-13 patch). PhotonVision letterboxes
// the colour frame to the model's input size (e.g. 640x640 BGR) and hands it here; this uploads
// it, converts it on the GPU to the network's input (RGB, 0..1, planar NCHW), runs the engine,
// and decodes the output on the CPU. Results are in letterboxed pixels; Java maps them back.
//
//   long   create(String enginePath)           engine file from trtexec (or Ultralytics .engine)
//   int[]  inputSize(long ptr)                 {width, height} of the input tensor
//   float[] detect(long ptr, long bgrMatPtr, double boxThresh, double nmsThresh, int numClasses)
//          -> flat [x, y, w, h, classId, confidence] per detection, best first
//   void   destroy(long ptr)
//
// Output layouts handled (Ultralytics export):
//   raw YOLOv8/YOLO11/YOLO26 (nms=None): [1, 4+nc, N], cx cy w h + per-class scores; NMS here
//   end-to-end (nms=True, or YOLO26 nms=False): [1, K, 6], x1 y1 x2 y2 score class
#include <jni.h>

#include <dlfcn.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include <cuda_fp16.h>
#include <cuda_runtime.h>
#include <NvInfer.h>
#include <opencv2/core/mat.hpp>

namespace {

class TrtLogger : public nvinfer1::ILogger {
  void log(Severity severity, const char *msg) noexcept override {
    if (severity <= Severity::kWARNING) std::cout << "TensorRT: " << msg << std::endl;
  }
};
TrtLogger gLogger;

__global__ void BgrToNchw(const uint8_t *bgr, int w, int h, float *out) {
  const int x = blockIdx.x * blockDim.x + threadIdx.x;
  const int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= w || y >= h) return;
  const int i = y * w + x;
  const int hw = w * h;
  out[i] = bgr[i * 3 + 2] / 255.f;           // R
  out[hw + i] = bgr[i * 3 + 1] / 255.f;      // G
  out[2 * hw + i] = bgr[i * 3 + 0] / 255.f;  // B
}

__global__ void BgrToNchwHalf(const uint8_t *bgr, int w, int h, __half *out) {
  const int x = blockIdx.x * blockDim.x + threadIdx.x;
  const int y = blockIdx.y * blockDim.y + threadIdx.y;
  if (x >= w || y >= h) return;
  const int i = y * w + x;
  const int hw = w * h;
  out[i] = __float2half(bgr[i * 3 + 2] / 255.f);
  out[hw + i] = __float2half(bgr[i * 3 + 1] / 255.f);
  out[2 * hw + i] = __float2half(bgr[i * 3 + 0] / 255.f);
}

struct Det {
  float x, y, w, h, score;
  int cls;
};

float Iou(const Det &a, const Det &b) {
  const float x1 = std::max(a.x, b.x), y1 = std::max(a.y, b.y);
  const float x2 = std::min(a.x + a.w, b.x + b.w), y2 = std::min(a.y + a.h, b.y + b.h);
  const float inter = std::max(0.f, x2 - x1) * std::max(0.f, y2 - y1);
  const float uni = a.w * a.h + b.w * b.h - inter;
  return uni > 0 ? inter / uni : 0.f;
}

struct Detector {
  std::unique_ptr<nvinfer1::IRuntime> runtime;
  std::unique_ptr<nvinfer1::ICudaEngine> engine;
  std::unique_ptr<nvinfer1::IExecutionContext> context;
  cudaStream_t stream = nullptr;
  std::string in_name, out_name;
  nvinfer1::DataType in_type = nvinfer1::DataType::kFLOAT;
  nvinfer1::DataType out_type = nvinfer1::DataType::kFLOAT;
  int in_w = 0, in_h = 0;
  nvinfer1::Dims out_dims{};
  size_t out_count = 0;
  void *d_in = nullptr;          // network input (float or half, NCHW)
  uint8_t *d_bgr = nullptr;      // uploaded letterboxed frame
  void *d_out = nullptr;         // network output
  std::vector<float> h_out;      // output on the host, as float
  std::vector<uint16_t> h_out16; // output on the host, if the engine emits half
  std::mutex mu;

  ~Detector() {
    if (d_in) cudaFree(d_in);
    if (d_bgr) cudaFree(d_bgr);
    if (d_out) cudaFree(d_out);
    if (stream) cudaStreamDestroy(stream);
  }
};

size_t Volume(const nvinfer1::Dims &d) {
  size_t v = 1;
  for (int i = 0; i < d.nbDims; ++i) v *= static_cast<size_t>(std::max<int64_t>(d.d[i], 1));
  return v;
}

std::string DimsStr(const nvinfer1::Dims &d) {
  std::string s = "[";
  for (int i = 0; i < d.nbDims; ++i) s += (i ? "," : "") + std::to_string(d.d[i]);
  return s + "]";
}

// Ultralytics' .engine files start with a 4-byte little-endian length and a JSON metadata block
// before the real TensorRT engine; trtexec's don't. Skip the header if it's there.
std::vector<char> ReadEngine(const std::string &path) {
  std::ifstream f(path, std::ios::binary);
  std::vector<char> buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  if (buf.size() > 8) {
    uint32_t len;
    std::memcpy(&len, buf.data(), 4);
    if (len > 2 && len + 4 < buf.size() && buf[4] == '{' && buf[4 + len - 1] == '}') {
      std::cout << "TensorRT: skipping Ultralytics metadata header (" << len << " bytes)"
                << std::endl;
      buf.erase(buf.begin(), buf.begin() + 4 + len);
    }
  }
  return buf;
}

std::vector<Det> Decode(Detector &d, double box_thresh, double nms_thresh, int num_classes) {
  std::vector<Det> dets;
  const float *o = d.h_out.data();
  const auto &dims = d.out_dims;
  const bool end_to_end = dims.nbDims == 3 && dims.d[2] == 6;
  if (end_to_end) {
    const int k = static_cast<int>(dims.d[1]);
    for (int i = 0; i < k; ++i) {
      const float *r = o + i * 6;
      if (r[4] < box_thresh) continue;
      dets.push_back({r[0], r[1], r[2] - r[0], r[3] - r[1], r[4], static_cast<int>(r[5])});
    }
    // Already NMS'd by the model.
  } else if (dims.nbDims == 3) {
    const int c = static_cast<int>(dims.d[1]);  // 4 + nc
    const int n = static_cast<int>(dims.d[2]);
    const int nc = std::max(1, std::min(num_classes > 0 ? num_classes : c - 4, c - 4));
    for (int i = 0; i < n; ++i) {
      int best = 0;
      float score = o[4 * n + i];
      for (int k = 1; k < nc; ++k) {
        const float s = o[(4 + k) * n + i];
        if (s > score) {
          score = s;
          best = k;
        }
      }
      if (score < box_thresh) continue;
      const float cx = o[i], cy = o[n + i], w = o[2 * n + i], h = o[3 * n + i];
      dets.push_back({cx - w / 2, cy - h / 2, w, h, score, best});
    }
    std::sort(dets.begin(), dets.end(), [](const Det &a, const Det &b) { return a.score > b.score; });
    std::vector<Det> kept;
    std::vector<bool> removed(dets.size(), false);
    for (size_t i = 0; i < dets.size(); ++i) {
      if (removed[i]) continue;
      kept.push_back(dets[i]);
      for (size_t j = i + 1; j < dets.size(); ++j) {
        if (!removed[j] && dets[j].cls == dets[i].cls && Iou(dets[i], dets[j]) > nms_thresh) {
          removed[j] = true;
        }
      }
    }
    dets.swap(kept);
  }
  std::sort(dets.begin(), dets.end(), [](const Det &a, const Det &b) { return a.score > b.score; });
  return dets;
}

// Holds lib971apriltag.so's CUDA capture lock shared for a scope (GpuDetectorJNI.cc,
// CudaCaptureLock): that library records CUDA graphs, and while it does, some CUDA calls here
// (cudaFree, cudaMalloc, engine loading) would break. Does nothing until that library is loaded.
class CudaShared {
 public:
  CudaShared() : unlock_(Fns().unlock) {
    if (auto lock = Fns().lock) lock();
  }
  ~CudaShared() {
    if (unlock_) unlock_();
  }

 private:
  using Fn = void (*)();
  struct Pair {
    Fn lock = nullptr, unlock = nullptr;
  };
  static Pair Fns() {
    static std::atomic<bool> found{false};
    static Pair fns;
    if (!found.load(std::memory_order_acquire)) {
      static std::mutex mu;
      std::lock_guard<std::mutex> l(mu);
      if (!found.load(std::memory_order_relaxed)) {
        if (void *h = dlopen("lib971apriltag.so", RTLD_NOLOAD | RTLD_LAZY)) {
          fns.lock = reinterpret_cast<Fn>(dlsym(h, "spectrum_cuda_lock_shared"));
          fns.unlock = reinterpret_cast<Fn>(dlsym(h, "spectrum_cuda_unlock_shared"));
          if (fns.lock && fns.unlock) found.store(true, std::memory_order_release);
          else fns = Pair{};
        }
      }
    }
    return found.load(std::memory_order_acquire) ? fns : Pair{};
  }
  Fn unlock_;
};

}  // namespace

extern "C" {

JNIEXPORT jlong JNICALL Java_org_photonvision_jni_TensorRtJNI_create(JNIEnv *env, jclass,
                                                                     jstring jpath) {
  CudaShared cuda;
  const char *cpath = env->GetStringUTFChars(jpath, nullptr);
  const std::string path(cpath);
  env->ReleaseStringUTFChars(jpath, cpath);

  auto d = std::make_unique<Detector>();
  const auto blob = ReadEngine(path);
  if (blob.empty()) {
    std::cout << "TensorRT: cannot read " << path << std::endl;
    return 0;
  }
  d->runtime.reset(nvinfer1::createInferRuntime(gLogger));
  if (!d->runtime) return 0;
  d->engine.reset(d->runtime->deserializeCudaEngine(blob.data(), blob.size()));
  if (!d->engine) {
    std::cout << "TensorRT: " << path
              << " is not a TensorRT engine for this device/version (rebuild it with trtexec here)"
              << std::endl;
    return 0;
  }
  d->context.reset(d->engine->createExecutionContext());
  if (!d->context) return 0;

  for (int i = 0; i < d->engine->getNbIOTensors(); ++i) {
    const char *name = d->engine->getIOTensorName(i);
    if (d->engine->getTensorIOMode(name) == nvinfer1::TensorIOMode::kINPUT) {
      d->in_name = name;
      d->in_type = d->engine->getTensorDataType(name);
      const auto dims = d->engine->getTensorShape(name);  // [1, 3, H, W]
      if (dims.nbDims != 4 || dims.d[1] != 3) {
        std::cout << "TensorRT: unexpected input shape " << DimsStr(dims) << std::endl;
        return 0;
      }
      d->in_h = static_cast<int>(dims.d[2]);
      d->in_w = static_cast<int>(dims.d[3]);
    } else if (d->out_name.empty()) {
      d->out_name = name;
      d->out_type = d->engine->getTensorDataType(name);
      d->out_dims = d->engine->getTensorShape(name);
    }
  }
  if (d->in_name.empty() || d->out_name.empty() || d->in_w <= 0 || d->in_h <= 0) {
    std::cout << "TensorRT: could not find the input/output tensors" << std::endl;
    return 0;
  }
  const bool in_half = d->in_type == nvinfer1::DataType::kHALF;
  const bool out_half = d->out_type == nvinfer1::DataType::kHALF;
  d->out_count = Volume(d->out_dims);
  if (cudaStreamCreate(&d->stream) != cudaSuccess ||
      cudaMalloc(&d->d_in, size_t(3) * d->in_w * d->in_h * (in_half ? 2 : 4)) != cudaSuccess ||
      cudaMalloc(reinterpret_cast<void **>(&d->d_bgr), size_t(3) * d->in_w * d->in_h) !=
          cudaSuccess ||
      cudaMalloc(&d->d_out, d->out_count * (out_half ? 2 : 4)) != cudaSuccess) {
    std::cout << "TensorRT: CUDA allocation failed" << std::endl;
    return 0;
  }
  d->h_out.resize(d->out_count);
  if (out_half) d->h_out16.resize(d->out_count);
  d->context->setTensorAddress(d->in_name.c_str(), d->d_in);
  d->context->setTensorAddress(d->out_name.c_str(), d->d_out);
  std::cout << "TensorRT: loaded " << path << ": input " << d->in_name << " " << d->in_w << "x"
            << d->in_h << (in_half ? " fp16" : " fp32") << ", output " << d->out_name << " "
            << DimsStr(d->out_dims) << (out_half ? " fp16" : " fp32") << std::endl;
  return reinterpret_cast<jlong>(d.release());
}

JNIEXPORT jintArray JNICALL Java_org_photonvision_jni_TensorRtJNI_inputSize(JNIEnv *env, jclass,
                                                                           jlong ptr) {
  auto *d = reinterpret_cast<Detector *>(ptr);
  jint v[2] = {d ? d->in_w : 0, d ? d->in_h : 0};
  jintArray a = env->NewIntArray(2);
  env->SetIntArrayRegion(a, 0, 2, v);
  return a;
}

JNIEXPORT jfloatArray JNICALL Java_org_photonvision_jni_TensorRtJNI_detect(
    JNIEnv *env, jclass, jlong ptr, jlong mat_ptr, jdouble box_thresh, jdouble nms_thresh,
    jint num_classes) {
  auto *d = reinterpret_cast<Detector *>(ptr);
  auto *mat = reinterpret_cast<cv::Mat *>(mat_ptr);
  if (!d || !mat) return nullptr;
  std::lock_guard<std::mutex> lock(d->mu);
  CudaShared cuda;
  if (mat->type() != CV_8UC3 || mat->cols != d->in_w || mat->rows != d->in_h ||
      !mat->isContinuous()) {
    std::cout << "TensorRT: need a continuous " << d->in_w << "x" << d->in_h
              << " BGR image, got " << mat->cols << "x" << mat->rows << " type " << mat->type()
              << std::endl;
    return nullptr;
  }
  const size_t bytes = size_t(3) * d->in_w * d->in_h;
  cudaMemcpyAsync(d->d_bgr, mat->data, bytes, cudaMemcpyHostToDevice, d->stream);
  const dim3 block(16, 16);
  const dim3 grid((d->in_w + 15) / 16, (d->in_h + 15) / 16);
  if (d->in_type == nvinfer1::DataType::kHALF) {
    BgrToNchwHalf<<<grid, block, 0, d->stream>>>(d->d_bgr, d->in_w, d->in_h,
                                                  static_cast<__half *>(d->d_in));
  } else {
    BgrToNchw<<<grid, block, 0, d->stream>>>(d->d_bgr, d->in_w, d->in_h,
                                              static_cast<float *>(d->d_in));
  }
  if (!d->context->enqueueV3(d->stream)) {
    std::cout << "TensorRT: enqueueV3 failed" << std::endl;
    return nullptr;
  }
  if (d->out_type == nvinfer1::DataType::kHALF) {
    cudaMemcpyAsync(d->h_out16.data(), d->d_out, d->out_count * 2, cudaMemcpyDeviceToHost,
                    d->stream);
  } else {
    cudaMemcpyAsync(d->h_out.data(), d->d_out, d->out_count * 4, cudaMemcpyDeviceToHost,
                    d->stream);
  }
  const cudaError_t err = cudaStreamSynchronize(d->stream);
  if (err != cudaSuccess) {
    std::cout << "TensorRT: " << cudaGetErrorString(err) << std::endl;
    cudaGetLastError();
    return nullptr;
  }
  if (d->out_type == nvinfer1::DataType::kHALF) {
    for (size_t i = 0; i < d->out_count; ++i) {
      __half_raw r;
      r.x = d->h_out16[i];
      d->h_out[i] = __half2float(__half(r));
    }
  }
  const auto dets = Decode(*d, box_thresh, nms_thresh, num_classes);
  std::vector<float> flat;
  flat.reserve(dets.size() * 6);
  for (const auto &t : dets) {
    flat.insert(flat.end(), {t.x, t.y, t.w, t.h, static_cast<float>(t.cls), t.score});
  }
  jfloatArray a = env->NewFloatArray(static_cast<jsize>(flat.size()));
  env->SetFloatArrayRegion(a, 0, static_cast<jsize>(flat.size()), flat.data());
  return a;
}

JNIEXPORT void JNICALL Java_org_photonvision_jni_TensorRtJNI_destroy(JNIEnv *, jclass, jlong ptr) {
  CudaShared cuda;
  delete reinterpret_cast<Detector *>(ptr);
}

}  // extern "C"
