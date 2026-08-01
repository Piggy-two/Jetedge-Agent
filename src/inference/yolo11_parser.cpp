// YOLO11s custom output parser for DeepStream nvinfer (Stage 4).
//
// DeepStream 7.1 ships no YOLO parser for raw `1x84x5040` output, so this
// shared library is loaded by nvinfer via `custom-lib-path` +
// `parse-bbox-func-name` and converts the model output into
// NvDsInferObjectDetectionInfo objects.
//
// Output layout (verified on the actual ONNX graph and with real-image
// inference on the Jetson):
//   output0: float[1][84][5040] — 84 channels per cell, 5040 cells.
//   Channels 0..3: (cx, cy, w, h) as ABSOLUTE pixel coordinates in the
//     model input space (384x640). The model graph already contains the
//     full decode: DFL Softmax → linear combination → stride multiply
//     (`/model.23/Mul_2`), so NO sigmoid/stride math is needed here.
//   Channels 4..83: class scores, ALREADY SIGMOIDED to [0, 1]
//     (`/model.23/Sigmoid`).
//
// The nvinfer plugin runs NMS clustering afterwards (cluster-mode=2) and
// scales coordinates from network resolution back to the source frame.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <iostream>

#include <cuda_fp16.h>

#include "nvdsinfer_custom_impl.h"

namespace {

// Model input resolution (matches the accepted ONNX export spec).
constexpr float kModelWidth  = 640.0f;
constexpr float kModelHeight = 384.0f;
constexpr int kNumCells      = 5040;
constexpr int kNumClasses    = 80;

inline float clampf(float v, float lo, float hi) {
  return std::max(lo, std::min(v, hi));
}

// Element accessor supporting both FP32 and FP16 output buffers (an FP16
// engine may expose the output tensor as HALF).
class TensorReader {
 public:
  TensorReader(const NvDsInferLayerInfo& layer)
      : fp32_(layer.dataType == FLOAT
                  ? static_cast<const float*>(layer.buffer)
                  : nullptr),
        fp16_(layer.dataType == HALF
                  ? static_cast<const __half*>(layer.buffer)
                  : nullptr) {}

  float at(int idx) const {
    if (fp32_) return fp32_[idx];
    if (fp16_) return __half2float(fp16_[idx]);
    return 0.0f;
  }

  bool valid() const { return fp32_ != nullptr || fp16_ != nullptr; }

 private:
  const float* fp32_;
  const __half* fp16_;
};

}  // namespace

extern "C"
bool NvDsInferParseCustomYolo11(
    std::vector<NvDsInferLayerInfo> const& outputLayersInfo,
    NvDsInferNetworkInfo const& networkInfo,
    NvDsInferParseDetectionParams const& detectionParams,
    std::vector<NvDsInferObjectDetectionInfo>& objectList) {
  // ---- Locate the "output0" layer -----------------------------------------
  const NvDsInferLayerInfo* out = nullptr;
  for (const auto& layer : outputLayersInfo) {
    if ((layer.dataType == FLOAT || layer.dataType == HALF) &&
        layer.layerName && std::strcmp(layer.layerName, "output0") == 0) {
      out = &layer;
      break;
    }
  }
  if (!out) {
    std::cerr << "[yolo11_parser] ERROR: output layer 'output0' not found ("
              << outputLayersInfo.size() << " layers reported)" << std::endl;
    return false;
  }

  // ---- Validate tensor dimensions ------------------------------------------
  // DeepStream reports the layer dims WITHOUT the batch dimension and does
  // not preserve the H/W split: d[0] = channels (84), d[1] = numCells (5040),
  // and inferDims.numElements = channels * numCells.  Do not rely on CHW.
  const NvDsInferDims& dims = out->inferDims;
  if (dims.numDims < 2 || dims.numElements == 0) {
    std::cerr << "[yolo11_parser] ERROR: unexpected output dims, numDims="
              << dims.numDims << " numElements=" << dims.numElements << std::endl;
    return false;
  }
  const int channels = static_cast<int>(dims.d[0]);
  const int numCells = static_cast<int>(dims.numElements) / channels;
  if (numCells != kNumCells || channels < kNumClasses + 4) {
    std::cerr << "[yolo11_parser] ERROR: unexpected output dims "
              << channels << "x" << numCells
              << " (expected 84x5040)" << std::endl;
    return false;
  }

  TensorReader data(*out);
  if (!data.valid()) {
    std::cerr << "[yolo11_parser] ERROR: output buffer is null or has an "
              << "unsupported data type" << std::endl;
    return false;
  }

  // Model input resolution (the engine input; source frames are scaled to
  // this space before inference, so coordinates must be reported in it).
  const float net_w = static_cast<float>(networkInfo.width);
  const float net_h = static_cast<float>(networkInfo.height);
  // Guard against a caller reporting 0 — fall back to the known model size.
  const float in_w = net_w > 0.0f ? net_w : kModelWidth;
  const float in_h = net_h > 0.0f ? net_h : kModelHeight;

  // ---- Decode all cells -----------------------------------------------------
  // Channel layout (channels are contiguous per cell index):
  //   ch0[i] = cx, ch1[i] = cy, ch2[i] = w, ch3[i] = h, ch4..83[i] = scores
  for (int i = 0; i < numCells; ++i) {
    const float cx = data.at(i);
    const float cy = data.at(kNumCells + i);
    const float w  = data.at(2 * kNumCells + i);
    const float h  = data.at(3 * kNumCells + i);

    // Best class: max over channels 4..83 (already sigmoided).
    float max_score = 0.0f;
    int best_class = 0;
    for (int c = 0; c < kNumClasses; ++c) {
      const float s = data.at((4 + c) * kNumCells + i);
      if (s > max_score) {
        max_score = s;
        best_class = c;
      }
    }

    if (best_class >= static_cast<int>(detectionParams.numClassesConfigured)) {
      continue;
    }
    if (max_score < detectionParams.perClassPreclusterThreshold[best_class]) {
      continue;
    }
    if (w <= 0.0f || h <= 0.0f) {
      continue;
    }

    // Center-format → left/top/width/height, clipped to network resolution.
    float left = clampf(cx - w * 0.5f, 0.0f, in_w - 1.0f);
    float top  = clampf(cy - h * 0.5f, 0.0f, in_h - 1.0f);
    float right  = clampf(cx + w * 0.5f, 0.0f, in_w - 1.0f);
    float bottom = clampf(cy + h * 0.5f, 0.0f, in_h - 1.0f);
    if (right <= left || bottom <= top) {
      continue;
    }

    NvDsInferObjectDetectionInfo obj{};
    obj.classId             = static_cast<unsigned int>(best_class);
    obj.detectionConfidence = max_score;
    obj.left   = left;
    obj.top    = top;
    obj.width  = right - left;
    obj.height = bottom - top;
    objectList.push_back(obj);
  }

  return true;
}

// Compile-time signature validation against the NvDsInferParseCustomFunc type.
CHECK_CUSTOM_PARSE_FUNC_PROTOTYPE(NvDsInferParseCustomYolo11);
