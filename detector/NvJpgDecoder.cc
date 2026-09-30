// SpectrumJetson: hardware (NVJPG) JPEG decode to 8-bit gray. See nvjpg_decoder.h.
//
// Measured on the Orin Nano (L4T R36.5.2, the Thriftiest Cam's 1280x800 4:2:2 JPEGs,
// 2026-09-24): 2.3 ms decode + 0.24 ms copy and 0.73 ms of CPU a frame, against 2.9 ms all on
// the CPU for libjpeg-turbo, with identical output. What we learned (docs/VISION-RESEARCH.md):
//   - Writing the rows straight into our buffer (raw_data_out, as AOS does) returned the FIRST
//     frame for every frame. So decode into libnvjpeg's own buffer (IsVendorbuf, NVIDIA's
//     NvJPEGDecoder::decodeToFd method) and copy its Y plane out.
//   - That buffer is uncached for the CPU (a 1 MB memcpy takes ~1.2 ms), so CUDA copies it.
//   - libnvjpeg cycles through 4 buffers behind the SAME fd number. Each one is told apart by
//     its dmabuf inode and gets its own CUDA registration; one registration for all of them
//     read stale frames.
//   - After a decode error the decompress object must be re-created, or every later frame fails.
//   - NVIDIA's NvJPEGDecoder class keeps libjpeg's default error_exit, which calls exit().
//   - Without mjpeg_decode = TRUE (MakeCinfo), libnvjpeg leaks ~250 KB a frame.

#include "nvjpg_decoder.h"

#include <sys/stat.h>

#include <csetjmp>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include <cuda.h>
#include <cuda_runtime.h>

#include "jpeglib.h"  // jetson_multimedia_api's libjpeg-8b: libnvjpeg's struct layout
#include "nvbufsurface.h"

#include <cudaEGL.h>  // after nvbufsurface.h; EGL_NO_X11 keeps X11's macros out

#define SNJ_API __attribute__((visibility("default")))

// nvjpg_bgr.cu
cudaError_t SnjInitBgrTables();
cudaError_t SnjYcc422ToBgr(const uint8_t *y, int y_pitch, const uint8_t *cb, const uint8_t *cr,
                           int c_pitch, uint8_t *out, int out_pitch, int width, int height,
                           int c_width, cudaStream_t stream);

namespace {

// libnvjpeg's state after a hardware jpeg_start_decompress (jpegint.h DSTATE_READY; NVIDIA's
// NvJpegDecoder.cpp checks the same).
constexpr int kDstateReady = 202;
// libnvjpeg uses 4 buffers; more than this means it reallocated them, so start over.
constexpr size_t kMaxBuffers = 8;

struct ErrorMgr {
  jpeg_error_mgr pub;
  jmp_buf jump;
  char message[JMSG_LENGTH_MAX];
};

void ErrorExit(j_common_ptr c) {  // the default calls exit(): never in a JVM
  auto *e = reinterpret_cast<ErrorMgr *>(c->err);
  (*c->err->format_message)(c, e->message);
  longjmp(e->jump, 1);
}
void Silent(j_common_ptr, int) {}

struct Buffer {
  ino_t ino;
  NvBufSurface *surf;
  CUgraphicsResource res;
  CUeglFrame frame;
};

thread_local std::string create_error;

}  // namespace

struct SnjDecoder {
  jpeg_decompress_struct cinfo;
  ErrorMgr err;
  bool have_cinfo = false;
  cudaStream_t stream = nullptr;
  std::vector<Buffer> buffers;
  // Size and sampling of the frames the buffers were made for. libnvjpeg reallocates its buffers
  // when these change, so we drop our registrations first, while the old buffers still exist.
  int format[5] = {0, 0, 0, 0, 0};
  // snj_decode_bgr's output on the GPU, before the copy into the caller's buffer.
  uint8_t *bgr = nullptr;
  size_t bgr_pitch = 0;
  int bgr_width = 0, bgr_height = 0;
  std::string error;
};

namespace {

// unmap=false only when libnvjpeg may already have freed the buffers (then only CUDA's side is
// released; touching the NvBufSurface could be a use-after-free).
void Unregister(SnjDecoder *d, bool unmap = true) {
  for (auto &b : d->buffers) {
    cuGraphicsUnregisterResource(b.res);
    if (unmap) NvBufSurfaceUnMapEglImage(b.surf, 0);
  }
  d->buffers.clear();
}

void MakeCinfo(SnjDecoder *d) {
  std::memset(&d->cinfo, 0, sizeof(d->cinfo));
  std::memset(&d->err, 0, sizeof(d->err));
  d->cinfo.err = jpeg_std_error(&d->err.pub);
  d->err.pub.error_exit = ErrorExit;
  d->err.pub.emit_message = Silent;
  jpeg_create_decompress(&d->cinfo);
  // Stream (MJPEG) mode, as NVIDIA's NvJPEGDecoder sets it. Without it, libnvjpeg leaks ~250 KB
  // of memory every frame until the decompress object is destroyed: PhotonVision grew to 5.6 GB
  // and was OOM-killed (2026-09-24). With it, a decoder takes ~85 MB once and stays flat
  // (13,000 frames measured), with the same decode time.
  d->cinfo.mjpeg_decode = TRUE;
  d->have_cinfo = true;
}

// Frees libnvjpeg's buffers (after dropping our registrations of them) and starts a fresh
// decompress object.
void Reset(SnjDecoder *d) {
  Unregister(d);
  if (d->have_cinfo) jpeg_destroy_decompress(&d->cinfo);
  MakeCinfo(d);
}

enum Step {
  kDecoded, kLibjpegError, kWrongSize, kHeaderUnsupported, kNot422, kNotHardware, kNewFormat
};

// The libjpeg part. Nothing here has a destructor, so longjmp out of libnvjpeg is safe.
// bgr: the caller needs 4:2:2 chroma (the only layout the BGR kernel converts).
Step DecodeToBuffer(SnjDecoder *d, const uint8_t *jpeg, size_t size, int width, int height,
                    bool bgr, int *fd) {
  jpeg_decompress_struct *c = &d->cinfo;
  NvBufSurface vendor;  // libnvjpeg's out-parameter, as in NvJPEGDecoder::decodeToFd
  if (setjmp(d->err.jump)) return kLibjpegError;
  jpeg_mem_src(c, const_cast<uint8_t *>(jpeg), size);
  jpeg_read_header(c, TRUE);
  if (static_cast<int>(c->image_width) != width || static_cast<int>(c->image_height) != height) {
    jpeg_abort_decompress(c);
    return kWrongSize;
  }
  if (c->progressive_mode || c->num_components != 3 || c->jpeg_color_space != JCS_YCbCr) {
    jpeg_abort_decompress(c);
    return kHeaderUnsupported;
  }
  if (bgr && (c->comp_info[0].h_samp_factor != 2 || c->comp_info[0].v_samp_factor != 1 ||
              c->comp_info[1].h_samp_factor != 1 || c->comp_info[1].v_samp_factor != 1 ||
              c->comp_info[2].h_samp_factor != 1 || c->comp_info[2].v_samp_factor != 1)) {
    jpeg_abort_decompress(c);
    return kNot422;
  }
  const int format[5] = {width, height, c->comp_info[0].h_samp_factor,
                         c->comp_info[0].v_samp_factor, c->max_v_samp_factor};
  if (std::memcmp(format, d->format, sizeof(format)) != 0) {
    std::memcpy(d->format, format, sizeof(format));
    if (!d->buffers.empty()) {
      jpeg_abort_decompress(c);
      return kNewFormat;  // the caller resets, then decodes again
    }
  }
  c->out_color_space = JCS_YCbCr;
  c->IsVendorbuf = TRUE;
  c->pVendor_buf = reinterpret_cast<unsigned char *>(&vendor);
  jpeg_start_decompress(c);
  if (c->global_state != kDstateReady) return kNotHardware;
  jpeg_read_raw_data(c, nullptr, c->comp_info[0].v_samp_factor * DCTSIZE);
  const bool hardware = c->tegra_acceleration;
  jpeg_finish_decompress(c);
  if (!hardware) return kNotHardware;
  *fd = c->fd;
  return kDecoded;
}

int Fail(SnjDecoder *d, int rc, const std::string &why) {
  d->error = why;
  return rc;
}

// Decodes on the NVJPG engine into libnvjpeg's own buffer. SNJ_OK and *fd, or an error code.
int Decode(SnjDecoder *d, const uint8_t *jpeg, size_t size, int width, int height, bool bgr,
           int *fd) {
  // A thread other than the creator needs the context made current first.
  thread_local bool context = cudaFree(nullptr) == cudaSuccess;
  if (!context) return Fail(d, SNJ_CUDA, "no CUDA context on this thread");
  Step step = DecodeToBuffer(d, jpeg, size, width, height, bgr, fd);
  if (step == kNewFormat) {
    Reset(d);
    step = DecodeToBuffer(d, jpeg, size, width, height, bgr, fd);
  }
  switch (step) {
    case kDecoded:
      return SNJ_OK;
    case kNewFormat:  // can't happen twice in a row: the format was just recorded
      return Fail(d, SNJ_UNSUPPORTED, "JPEG format changed");
    case kLibjpegError: {
      std::string why = d->err.message;
      Reset(d);
      return Fail(d, SNJ_BAD_JPEG, why);
    }
    case kWrongSize:
      return Fail(d, SNJ_WRONG_SIZE, "JPEG size doesn't match");
    case kHeaderUnsupported:
      return Fail(d, SNJ_UNSUPPORTED, "not a baseline 3-component YCbCr JPEG");
    case kNot422:
      return Fail(d, SNJ_UNSUPPORTED, "colour decode needs 4:2:2 chroma");
    case kNotHardware:
      Reset(d);  // libnvjpeg's state after a non-hardware decode is unknown
      return Fail(d, SNJ_UNSUPPORTED, "libnvjpeg didn't use the NVJPG engine");
  }
  return Fail(d, SNJ_BAD_JPEG, "unreachable");
}

// libnvjpeg's buffer `fd` as seen by CUDA, registered once per buffer.
int MapBuffer(SnjDecoder *d, int fd, int width, int height, NvBufSurface **surf_out,
              Buffer **out) {
  struct stat sb;
  NvBufSurface *surf = nullptr;
  if (fstat(fd, &sb) != 0 || NvBufSurfaceFromFd(fd, reinterpret_cast<void **>(&surf)) != 0 ||
      !surf || surf->numFilled < 1) {
    return Fail(d, SNJ_CUDA, "libnvjpeg's output buffer isn't a valid NvBufSurface");
  }
  const NvBufSurfaceParams &p = surf->surfaceList[0];
  if (p.layout != NVBUF_LAYOUT_PITCH || static_cast<int>(p.width) < width ||
      static_cast<int>(p.height) < height) {
    return Fail(d, SNJ_UNSUPPORTED, "libnvjpeg's output buffer isn't pitch-linear WxH");
  }

  Buffer *b = nullptr;
  for (auto &x : d->buffers) {
    if (x.ino == sb.st_ino && x.surf == surf) b = &x;
  }
  if (!b) {
    // Unexpected (the format didn't change): libnvjpeg may have freed the old buffers already.
    if (d->buffers.size() >= kMaxBuffers) Unregister(d, /*unmap=*/false);
    if (NvBufSurfaceMapEglImage(surf, 0) != 0) {
      return Fail(d, SNJ_CUDA, "NvBufSurfaceMapEglImage failed");
    }
    Buffer nb{sb.st_ino, surf, nullptr, {}};
    CUresult r = cuGraphicsEGLRegisterImage(&nb.res, surf->surfaceList[0].mappedAddr.eglImage,
                                            CU_GRAPHICS_MAP_RESOURCE_FLAGS_NONE);
    if (r == CUDA_SUCCESS) r = cuGraphicsResourceGetMappedEglFrame(&nb.frame, nb.res, 0, 0);
    if (r != CUDA_SUCCESS || nb.frame.frameType != CU_EGL_FRAME_TYPE_PITCH) {
      if (nb.res) cuGraphicsUnregisterResource(nb.res);
      NvBufSurfaceUnMapEglImage(surf, 0);
      return Fail(d, SNJ_CUDA, "CUDA couldn't map libnvjpeg's buffer (CUresult " +
                                   std::to_string(r) + ")");
    }
    d->buffers.push_back(nb);
    b = &d->buffers.back();
  }
  *surf_out = surf;
  *out = b;
  return SNJ_OK;
}

int Finish(SnjDecoder *d, const char *what) {
  const cudaError_t e = cudaStreamSynchronize(d->stream);
  if (e != cudaSuccess) {
    cudaGetLastError();
    Unregister(d);
    return Fail(d, SNJ_CUDA, std::string(what) + " failed: " + cudaGetErrorString(e));
  }
  return SNJ_OK;
}

// Copies the Y plane of libnvjpeg's buffer `fd` into gray with CUDA.
int CopyGray(SnjDecoder *d, int fd, uint8_t *gray, int width, int height, size_t stride) {
  NvBufSurface *surf;
  Buffer *b;
  if (int rc = MapBuffer(d, fd, width, height, &surf, &b); rc != SNJ_OK) return rc;
  cudaMemcpy2DAsync(gray, stride, b->frame.frame.pPitch[0], b->frame.pitch, width, height,
                    cudaMemcpyDeviceToHost, d->stream);
  return Finish(d, "CUDA copy");
}

// The same, but the Y plane goes into gray_dev (width x height, rows width bytes apart, device
// memory) first, and from there into gray, so both hold the same pixels.
int CopyGrayDev(SnjDecoder *d, int fd, uint8_t *gray, int width, int height, size_t stride,
                uint8_t *gray_dev) {
  NvBufSurface *surf;
  Buffer *b;
  if (int rc = MapBuffer(d, fd, width, height, &surf, &b); rc != SNJ_OK) return rc;
  cudaMemcpy2DAsync(gray_dev, width, b->frame.frame.pPitch[0], b->frame.pitch, width, height,
                    cudaMemcpyDeviceToDevice, d->stream);
  cudaMemcpy2DAsync(gray, stride, gray_dev, width, width, height, cudaMemcpyDeviceToHost,
                    d->stream);
  return Finish(d, "CUDA copy");
}

// Converts libnvjpeg's 4:2:2 planes to BGR on the GPU (nvjpg_bgr.cu), then copies that out.
int CopyBgr(SnjDecoder *d, int fd, uint8_t *bgr, int width, int height, size_t stride) {
  NvBufSurface *surf;
  Buffer *b;
  if (int rc = MapBuffer(d, fd, width, height, &surf, &b); rc != SNJ_OK) return rc;
  const NvBufSurfacePlaneParams &pp = surf->surfaceList[0].planeParams;
  const int c_width = (width + 1) / 2;
  if (surf->surfaceList[0].colorFormat != NVBUF_COLOR_FORMAT_YUV422 || pp.num_planes != 3 ||
      b->frame.planeCount != 3 || static_cast<int>(pp.width[1]) != c_width ||
      static_cast<int>(pp.height[1]) < height || pp.pitch[1] != pp.pitch[2]) {
    return Fail(d, SNJ_UNSUPPORTED, "libnvjpeg's buffer isn't planar 4:2:2");
  }
  if (d->bgr_width != width || d->bgr_height != height) {
    cudaFree(d->bgr);
    d->bgr = nullptr;
    d->bgr_width = d->bgr_height = 0;
    if (cudaError_t e = cudaMallocPitch(reinterpret_cast<void **>(&d->bgr), &d->bgr_pitch,
                                        static_cast<size_t>(width) * 3, height);
        e != cudaSuccess) {
      d->bgr = nullptr;
      return Fail(d, SNJ_CUDA, std::string("cudaMallocPitch failed: ") + cudaGetErrorString(e));
    }
    d->bgr_width = width;
    d->bgr_height = height;
  }
  if (cudaError_t e = SnjYcc422ToBgr(
          static_cast<const uint8_t *>(b->frame.frame.pPitch[0]), pp.pitch[0],
          static_cast<const uint8_t *>(b->frame.frame.pPitch[1]),
          static_cast<const uint8_t *>(b->frame.frame.pPitch[2]), pp.pitch[1], d->bgr,
          static_cast<int>(d->bgr_pitch), width, height, c_width, d->stream);
      e != cudaSuccess) {
    cudaGetLastError();
    return Fail(d, SNJ_CUDA, std::string("BGR kernel launch failed: ") + cudaGetErrorString(e));
  }
  cudaMemcpy2DAsync(bgr, stride, d->bgr, d->bgr_pitch, static_cast<size_t>(width) * 3, height,
                    cudaMemcpyDeviceToHost, d->stream);
  return Finish(d, "CUDA BGR conversion");
}

}  // namespace

extern "C" {

SNJ_API SnjDecoder *snj_create(void) {
  create_error.clear();
  // The runtime makes the primary context current on this thread; the driver-API calls
  // (EGL registration) need that.
  if (cudaError_t e = cudaFree(nullptr); e != cudaSuccess) {
    create_error = std::string("CUDA unavailable: ") + cudaGetErrorString(e);
    return nullptr;
  }
  static const cudaError_t tables = SnjInitBgrTables();  // once per process
  if (tables != cudaSuccess) {
    create_error = std::string("BGR tables upload failed: ") + cudaGetErrorString(tables);
    return nullptr;
  }
  auto *d = new SnjDecoder;
  if (cudaError_t e = cudaStreamCreateWithFlags(&d->stream, cudaStreamNonBlocking);
      e != cudaSuccess) {
    create_error = std::string("cudaStreamCreate failed: ") + cudaGetErrorString(e);
    delete d;
    return nullptr;
  }
  MakeCinfo(d);
  return d;
}

SNJ_API const char *snj_create_error(void) { return create_error.c_str(); }

SNJ_API void snj_destroy(SnjDecoder *d) {
  if (!d) return;
  Unregister(d);
  if (d->have_cinfo) jpeg_destroy_decompress(&d->cinfo);
  cudaFree(d->bgr);
  if (d->stream) cudaStreamDestroy(d->stream);
  delete d;
}

SNJ_API int snj_decode_gray(SnjDecoder *d, const uint8_t *jpeg, size_t size, uint8_t *gray,
                            int width, int height, size_t stride) {
  if (!d) return SNJ_BAD_JPEG;
  if (!jpeg || size < 4 || !gray || width <= 0 || height <= 0 ||
      stride < static_cast<size_t>(width)) {
    return Fail(d, SNJ_BAD_JPEG, "bad arguments");
  }
  int fd = -1;
  if (int rc = Decode(d, jpeg, size, width, height, /*bgr=*/false, &fd); rc != SNJ_OK) return rc;
  return CopyGray(d, fd, gray, width, height, stride);
}

SNJ_API int snj_decode_gray_dev(SnjDecoder *d, const uint8_t *jpeg, size_t size, uint8_t *gray,
                                int width, int height, size_t stride, uint8_t *gray_dev) {
  if (!d) return SNJ_BAD_JPEG;
  if (!jpeg || size < 4 || !gray || !gray_dev || width <= 0 || height <= 0 ||
      stride < static_cast<size_t>(width)) {
    return Fail(d, SNJ_BAD_JPEG, "bad arguments");
  }
  int fd = -1;
  if (int rc = Decode(d, jpeg, size, width, height, /*bgr=*/false, &fd); rc != SNJ_OK) return rc;
  return CopyGrayDev(d, fd, gray, width, height, stride, gray_dev);
}

SNJ_API int snj_decode_bgr(SnjDecoder *d, const uint8_t *jpeg, size_t size, uint8_t *bgr,
                           int width, int height, size_t stride) {
  if (!d) return SNJ_BAD_JPEG;
  if (!jpeg || size < 4 || !bgr || width <= 0 || height <= 0 ||
      stride < static_cast<size_t>(width) * 3) {
    return Fail(d, SNJ_BAD_JPEG, "bad arguments");
  }
  int fd = -1;
  if (int rc = Decode(d, jpeg, size, width, height, /*bgr=*/true, &fd); rc != SNJ_OK) return rc;
  return CopyBgr(d, fd, bgr, width, height, stride);
}

SNJ_API const char *snj_error(const SnjDecoder *d) { return d ? d->error.c_str() : ""; }

}  // extern "C"
