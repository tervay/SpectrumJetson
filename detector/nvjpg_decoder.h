// SpectrumJetson: hardware (NVJPG) JPEG decode to 8-bit gray, as its own library
// (libspectrumnvjpg.so, NvJpgDecoder.cc). GpuDetectorJNI.cc loads it with dlopen.
//
// Why a separate library: NVIDIA's libnvjpeg.so is a modified libjpeg-8b that exports the same
// jpeg_* names as libjpeg-turbo, with a different jpeg_decompress_struct. lib971apriltag.so links
// libjpeg-turbo for its CPU decode, so it can't also link libnvjpeg. Loaded with RTLD_DEEPBIND,
// this library's jpeg_* calls bind to libnvjpeg and the detector's stay with libjpeg-turbo.
//
// A decoder is not thread-safe: use one per camera thread.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SnjDecoder SnjDecoder;

enum {
  SNJ_OK = 0,
  SNJ_BAD_JPEG = -1,     // libjpeg error; the decoder was reset (the next frame decodes normally)
  SNJ_WRONG_SIZE = -2,   // the JPEG isn't width x height
  SNJ_UNSUPPORTED = -3,  // not a baseline 3-component YCbCr JPEG, or libnvjpeg didn't use the engine
  SNJ_CUDA = -4,         // copying the result out failed
};

// Returns NULL on failure (snj_create_error() says why).
SnjDecoder *snj_create(void);
const char *snj_create_error(void);
void snj_destroy(SnjDecoder *d);

// Decodes a JPEG's Y (brightness) channel into gray (width x height, rows `stride` bytes apart).
// The output is identical to libjpeg-turbo's JCS_GRAYSCALE with JDCT_ISLOW.
int snj_decode_gray(SnjDecoder *d, const uint8_t *jpeg, size_t size, uint8_t *gray, int width,
                    int height, size_t stride);

// snj_decode_gray, and the same pixels also left in gray_dev: device memory, width x height, rows
// width bytes apart (for the CUDA AprilTag detector, which then needn't copy the frame back up).
int snj_decode_gray_dev(SnjDecoder *d, const uint8_t *jpeg, size_t size, uint8_t *gray, int width,
                        int height, size_t stride, uint8_t *gray_dev);
// Decodes a 4:2:2 JPEG to 8-bit BGR (width x height, rows `stride` bytes apart), converting on
// the GPU (nvjpg_bgr.cu). Identical to libjpeg-turbo's JCS_EXT_BGR with its default settings
// (what cscore's cv::imdecode produces). Other chroma layouts return SNJ_UNSUPPORTED.
int snj_decode_bgr(SnjDecoder *d, const uint8_t *jpeg, size_t size, uint8_t *bgr, int width,
                   int height, size_t stride);

// The last error message for d ("" if none).
const char *snj_error(const SnjDecoder *d);

#ifdef __cplusplus
}
#endif
