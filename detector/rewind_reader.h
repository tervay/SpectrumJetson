// SpectrumJetson: reading a Rewind recording (docs/REWIND.md): a camera folder's frame index and
// JPEGs, decoded to gray with libjpeg-turbo exactly as GpuDetectorJNI.cc's TurboDecode does. Shared
// by fieldcal_detect (field calibration) and far_replay (the far-tag search on recordings).
#pragma once

#include <algorithm>
#include <cctype>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include <jpeglib.h>

namespace recording {
namespace fs = std::filesystem;

struct Frame {
  int index;
  long long jetson_us;
  fs::path mjpeg;
  long long offset, size;
  int width, height;
};

struct JpegError {
  jpeg_error_mgr mgr;
  std::jmp_buf jump;
};
inline void JpegErrorExit(j_common_ptr c) { std::longjmp(reinterpret_cast<JpegError *>(c->err)->jump, 1); }
inline void JpegQuiet(j_common_ptr, int) {}

// libjpeg-turbo to 8-bit gray, as GpuDetectorJNI.cc's TurboDecode does. false on a bad JPEG.
inline bool DecodeGray(const std::vector<unsigned char> &jpeg, std::vector<uint8_t> &out, int width, int height) {
  jpeg_decompress_struct c;
  JpegError err;
  c.err = jpeg_std_error(&err.mgr);
  err.mgr.error_exit = JpegErrorExit;
  err.mgr.emit_message = JpegQuiet;
  if (setjmp(err.jump)) {
    jpeg_destroy_decompress(&c);
    return false;
  }
  jpeg_create_decompress(&c);
  jpeg_mem_src(&c, const_cast<unsigned char *>(jpeg.data()), jpeg.size());
  if (jpeg_read_header(&c, TRUE) != JPEG_HEADER_OK) {
    jpeg_destroy_decompress(&c);
    return false;
  }
  c.out_color_space = JCS_GRAYSCALE;
  c.dct_method = JDCT_ISLOW;
  jpeg_start_decompress(&c);
  if (static_cast<int>(c.output_width) != width || static_cast<int>(c.output_height) != height ||
      c.output_components != 1) {
    jpeg_abort_decompress(&c);
    jpeg_destroy_decompress(&c);
    return false;
  }
  out.resize(static_cast<size_t>(width) * height);
  while (c.output_scanline < c.output_height) {
    JSAMPROW row = out.data() + static_cast<size_t>(c.output_scanline) * width;
    jpeg_read_scanlines(&c, &row, 1);
  }
  jpeg_finish_decompress(&c);
  jpeg_destroy_decompress(&c);
  return true;
}

// Every frame of a camera folder, in order (the Jetson writes "# frame,offset,size,width,height,
// jetson_us,robot_us"). A frame past the end of its .mjpeg (recording cut mid-write) ends a segment.
inline std::vector<Frame> ReadIndex(const fs::path &dir) {
  std::vector<fs::path> csvs;
  for (auto &e : fs::directory_iterator(dir)) {
    const auto name = e.path().filename().string();
    if (e.path().extension() == ".csv" && !name.empty() && std::isdigit(static_cast<unsigned char>(name[0])))
      csvs.push_back(e.path());
  }
  std::sort(csvs.begin(), csvs.end());
  std::vector<Frame> frames;
  int n = 0;
  for (const auto &csv : csvs) {
    fs::path mj = csv;
    mj.replace_extension(".mjpeg");
    if (!fs::exists(mj)) continue;
    const long long length = static_cast<long long>(fs::file_size(mj));
    std::ifstream in(csv);
    std::string line;
    while (std::getline(in, line)) {
      if (line.empty() || line[0] == '#' || !std::isdigit(static_cast<unsigned char>(line[0]))) continue;
      std::stringstream ss(line);
      std::string f[7];
      for (int i = 0; i < 7 && std::getline(ss, f[i], ','); ++i) {
      }
      Frame fr{n++, std::atoll(f[5].c_str()), mj, std::atoll(f[1].c_str()), std::atoll(f[2].c_str()),
               std::atoi(f[3].c_str()), std::atoi(f[4].c_str())};
      if (fr.offset + fr.size > length) break;
      frames.push_back(fr);
    }
  }
  return frames;
}

// A frame's JPEG bytes. false if the file can't be read.
inline bool ReadJpeg(const Frame &f, std::vector<unsigned char> &out) {
  std::ifstream in(f.mjpeg, std::ios::binary);
  if (!in) return false;
  in.seekg(f.offset);
  out.resize(static_cast<size_t>(f.size));
  return static_cast<bool>(in.read(reinterpret_cast<char *>(out.data()), f.size));
}

}  // namespace recording
