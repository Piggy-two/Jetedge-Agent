// Unit tests for the raw RGB frame reader used by the INT8 calibrator
// (Stage 13).  Pure logic — no TensorRT or CUDA dependency.

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "jetedge/calib/image_reader.h"

using jetedge::calib::kFrameBytes;
using jetedge::calib::kNetScaleFactor;
using jetedge::calib::kModelHeight;
using jetedge::calib::kModelWidth;
using jetedge::calib::listRawFiles;
using jetedge::calib::rgbToChw;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                     \
  do {                                                                  \
    ++g_checks;                                                         \
    if (!(cond)) {                                                      \
      std::printf("FAIL line %d: %s\n", __LINE__, #cond);               \
      ++g_failures;                                                     \
    }                                                                   \
  } while (0)

// Build a deterministic raw RGB frame: pixel (y, x) gets channel values
// r = (y*640+x) % 256, g = (y*640+x+1) % 256, b = (y*640+x+2) % 256.
static void makeFrame(unsigned char* buf) {
  for (int y = 0; y < kModelHeight; ++y) {
    for (int x = 0; x < kModelWidth; ++x) {
      const int v = y * kModelWidth + x;
      const size_t off = static_cast<size_t>(y) * kModelWidth + x;
      buf[off] = static_cast<unsigned char>(v % 256);                      // R
      buf[kFrameBytes / 3 + off] = static_cast<unsigned char>((v + 1) % 256);   // G
      buf[2 * kFrameBytes / 3 + off] = static_cast<unsigned char>((v + 2) % 256);  // B
    }
  }
}

static void test_hwc_to_chw_layout() {
  // Channel layout: HWC uint8 -> CHW float with pixel × scale.
  const int plane = kModelWidth * kModelHeight;
  std::vector<unsigned char> raw(static_cast<size_t>(kFrameBytes));
  makeFrame(raw.data());

  // Overwrite known pixels to verify the CHW offsets precisely.
  raw[0] = 255;                     // R(0,0)
  raw[plane] = 128;                 // G(0,0)
  raw[2 * plane] = 64;              // B(0,0)
  const size_t p57 = 5 * 640 + 7;   // (y=5,x=7)
  raw[p57] = 1;
  raw[plane + p57] = 2;
  raw[2 * plane + p57] = 3;

  const char* path = "/tmp/jetedge_image_reader_test.rgb";
  FILE* fh = std::fopen(path, "wb");
  CHECK(fh != nullptr);
  if (fh) {
    std::fwrite(raw.data(), 1, raw.size(), fh);
    std::fclose(fh);
  }

  std::vector<float> out;
  rgbToChw(path, kNetScaleFactor, out);
  CHECK(out.size() == static_cast<size_t>(kFrameBytes));

  const float eps = 1e-6f;
  // CHW: R plane [0, plane), G plane [plane, 2*plane), B plane [2*plane, 3*plane).
  CHECK(out[0] > 254.0f * kNetScaleFactor - eps);        // R(0,0)=255
  CHECK(out[plane] > 127.0f * kNetScaleFactor - eps);    // G(0,0)=128
  CHECK(out[2 * plane] > 63.0f * kNetScaleFactor - eps); // B(0,0)=64
  // (5,7): plane offset 5*640+7
  CHECK(out[p57] > 0.99f * kNetScaleFactor - eps);       // R(5,7)=1
  CHECK(out[plane + p57] > 1.99f * kNetScaleFactor - eps);
  CHECK(out[2 * plane + p57] > 2.99f * kNetScaleFactor - eps);

  std::remove(path);
}

static void test_bad_size_rejected() {
  const char* path = "/tmp/jetedge_image_reader_bad.rgb";
  FILE* fh = std::fopen(path, "wb");
  CHECK(fh != nullptr);
  if (fh) {
    std::fwrite("short", 1, 5, fh);
    std::fclose(fh);
  }
  std::vector<float> out;
  bool threw = false;
  try {
    rgbToChw(path, kNetScaleFactor, out);
  } catch (const std::runtime_error&) {
    threw = true;
  }
  CHECK(threw);
  CHECK(out.empty());  // nothing appended on failure
  std::remove(path);
}

static void test_missing_file_throws() {
  std::vector<float> out;
  bool threw = false;
  try {
    rgbToChw("/tmp/jetedge_does_not_exist_xyz.rgb", kNetScaleFactor, out);
  } catch (const std::runtime_error&) {
    threw = true;
  }
  CHECK(threw);
}

static void test_list_raw_files_sorted() {
  const char* dir = "/tmp/jetedge_image_reader_dir";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  for (const char* n : {"b_f2.rgb", "a_f10.rgb", "a_f2.rgb", "skip.txt"}) {
    std::ofstream(std::filesystem::path(dir) / n) << "x";
  }
  const auto files = listRawFiles(dir);
  CHECK(files.size() == 3);
  // Lexicographic order: a_f10.rgb < a_f2.rgb < b_f2.rgb
  if (files.size() == 3) {
    CHECK(files[0].find("a_f10.rgb") != std::string::npos);
    CHECK(files[1].find("a_f2.rgb") != std::string::npos);
    CHECK(files[2].find("b_f2.rgb") != std::string::npos);
  }
  std::filesystem::remove_all(dir);
}

static void test_missing_dir_throws() {
  bool threw = false;
  try {
    listRawFiles("/tmp/jetedge_no_such_dir_xyz");
  } catch (const std::runtime_error&) {
    threw = true;
  }
  CHECK(threw);
}

int main() {
  test_hwc_to_chw_layout();
  test_bad_size_rejected();
  test_missing_file_throws();
  test_list_raw_files_sorted();
  test_missing_dir_throws();

  std::printf("image_reader: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
