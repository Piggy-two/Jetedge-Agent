// Raw RGB frame reading for INT8 calibration (Stage 13).
//
// The calibration frames are extracted by scripts/extract_calib_frames.py
// as raw HWC uint8 RGB blobs (640x384x3 = 737280 B), so the calibrator can
// read them with fstream only — no OpenCV dependency.  The 1/255
// normalization matches nvinfer's net-scale-factor applied to the model
// input.

#ifndef JETEDGE_CALIB_IMAGE_READER_H
#define JETEDGE_CALIB_IMAGE_READER_H

#include <cstddef>
#include <stdexcept>
#include <string>
#include <vector>

namespace jetedge::calib {

// Model input resolution (accepted ONNX export spec).
inline constexpr int kModelWidth = 640;
inline constexpr int kModelHeight = 384;
inline constexpr int kModelChannels = 3;
inline constexpr int kFrameBytes = kModelWidth * kModelHeight * kModelChannels;

// nvinfer net-scale-factor for this model: 1/255.
inline constexpr float kNetScaleFactor = 0.00392156862745098f;

// Collect the *.rgb file paths under `dir`, sorted lexicographically.
// Throws std::runtime_error if the directory does not exist.
std::vector<std::string> listRawFiles(const std::string& dir);

// Read one raw RGB HWC uint8 frame and append its CHW float data (each
// pixel × `scale`) to `out`.  Throws std::runtime_error if the file is
// missing or its size differs from kFrameBytes.
void rgbToChw(const std::string& path, float scale, std::vector<float>& out);

}  // namespace jetedge::calib

#endif  // JETEDGE_CALIB_IMAGE_READER_H
