// Implementation of raw RGB frame reading (see image_reader.h).

#include "jetedge/calib/image_reader.h"

#include <algorithm>
#include <cstdio>
#include <filesystem>

namespace fs = std::filesystem;

namespace jetedge::calib {

std::vector<std::string> listRawFiles(const std::string& dir) {
  fs::path root(dir);
  if (!fs::is_directory(root)) {
    throw std::runtime_error("image_reader: not a directory: " + dir);
  }
  std::vector<std::string> files;
  for (const auto& entry : fs::directory_iterator(root)) {
    if (entry.is_regular_file() && entry.path().extension() == ".rgb") {
      files.push_back(entry.path().string());
    }
  }
  std::sort(files.begin(), files.end());
  return files;
}

void rgbToChw(const std::string& path, float scale, std::vector<float>& out) {
  std::vector<unsigned char> buf;
  buf.resize(kFrameBytes);
  FILE* fh = std::fopen(path.c_str(), "rb");
  if (!fh) {
    throw std::runtime_error("image_reader: cannot open: " + path);
  }
  const size_t got = std::fread(buf.data(), 1, buf.size(), fh);
  std::fclose(fh);
  if (got != buf.size()) {
    throw std::runtime_error("image_reader: unexpected size " +
                             std::to_string(got) + " for " + path +
                             " (expected " + std::to_string(kFrameBytes) + ")");
  }

  // HWC uint8 RGB -> CHW float, pixel × scale.  Channel data for a
  // fixed (y, x) sits at offset ch * (H*W) + y * W + x in the output.
  out.reserve(out.size() + static_cast<size_t>(kFrameBytes));
  const size_t plane = static_cast<size_t>(kModelWidth) * kModelHeight;
  for (int ch = 0; ch < kModelChannels; ++ch) {
    for (size_t i = 0; i < plane; ++i) {
      const unsigned char v = buf[ch * plane + i];
      out.push_back(static_cast<float>(v) * scale);
    }
  }
}

}  // namespace jetedge::calib
