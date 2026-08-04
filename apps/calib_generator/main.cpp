// INT8 calibration cache generator for YOLO11s (Stage 13).
//
// Runs one TensorRT build over the extracted calibration frames with an
// EntropyCalibrator2 and writes the standard TRT calibration cache text
// file (first line `TRT-<ver>-EntropyCalibration2`).  The serialized engine
// produced as a side effect is discarded — the deliverable engine is built
// by trtexec with `--int8 --calib=<cache>` (project convention, see
// docs/development_log.md), which keeps the engine build recordable and
// auditable.
//
// The build runs for ~10-15 min on the Jetson; calibration happens inside
// the build and the cache is written by the writeCalibrationCache callback.
//
// Usage:
//   calib_generator --onnx=<yolo11s_dynamic.onnx> \
//     --cache-out=<yolo11s_b4_384x640_int8.calib> \
//     --images=<raw frame dir> [--batch=4] [--workspace-mb=2048]
//
// Input frames: raw RGB HWC uint8 640x384x3 extracted by
// scripts/extract_calib_frames.py (see image_reader.h).

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include <NvInfer.h>
#include <NvOnnxParser.h>
#include <cuda_runtime_api.h>

#include "jetedge/calib/image_reader.h"

namespace {

class CalibLogger final : public nvinfer1::ILogger {
 public:
  void log(Severity severity, const char* msg) noexcept override {
    if (severity <= Severity::kWARNING) {
      if (severity == Severity::kWARNING) ++warnings_;
      std::fprintf(stderr, "[calib_generator] %s\n", msg);
    }
  }
  int warnings() const { return warnings_; }

 private:
  int warnings_ = 0;
};

// EntropyCalibrator2 (default) or MinMax calibrator over the raw RGB
// frames; the algorithm is selected by the template base class.  One
// contiguous device buffer holds the whole batch (batch x 3x384x640
// floats) — the calibrator contract is a single buffer per input binding,
// sized as batch x CxHxW (a single-frame buffer caused a device-memory
// overrun on the early runs).
template <typename CalibBase>
class CalibDataT final : public CalibBase {
 public:
  CalibDataT(std::vector<std::string> files, int batch, std::string cache_out)
      : files_(std::move(files)),
        batch_(batch),
        cache_out_(std::move(cache_out)) {
    if (cudaMalloc(&dev_buf_, devBytes() * static_cast<size_t>(batch_)) !=
        cudaSuccess) {
      throw std::runtime_error("cudaMalloc failed for calibration buffer");
    }
  }

  ~CalibDataT() override {
    if (dev_buf_) cudaFree(dev_buf_);
  }

  static std::size_t devBytes() {
    const std::size_t plane = static_cast<std::size_t>(
                                  jetedge::calib::kModelWidth) *
                              jetedge::calib::kModelHeight;
    return plane * jetedge::calib::kModelChannels * sizeof(float);
  }

  int getBatchSize() const noexcept override { return batch_; }

  bool getBatch(void* bindings[], char const* names[],
                int32_t nbBindings) noexcept override {
    (void)names;
    (void)nbBindings;
    if (next_idx_ + static_cast<size_t>(batch_) > files_.size()) {
      return false;  // fewer than a full batch of frames left — end of data
    }
    // One contiguous host buffer: batch frames, each CHW float x1/255.
    std::vector<float> host;
    try {
      for (int i = 0; i < batch_; ++i) {
        jetedge::calib::rgbToChw(files_[next_idx_ + static_cast<size_t>(i)],
                                 jetedge::calib::kNetScaleFactor, host);
      }
    } catch (const std::exception& e) {
      std::fprintf(stderr, "[calib_generator] %s\n", e.what());
      return false;
    }
    const std::size_t buf_bytes = devBytes() * static_cast<size_t>(batch_);
    const cudaError_t err = cudaMemcpy(dev_buf_, host.data(), buf_bytes,
                                       cudaMemcpyHostToDevice);
    if (err != cudaSuccess) {
      std::fprintf(stderr, "[calib_generator] cudaMemcpy failed: %s (%d)\n",
                   cudaGetErrorString(err), static_cast<int>(err));
      return false;
    }
    if (nbBindings < 1) {
      std::fprintf(stderr, "[calib_generator] no input bindings\n");
      return false;
    }
    bindings[0] = dev_buf_;
    next_idx_ += static_cast<size_t>(batch_);
    ++batches_;
    return true;
  }

  void const* readCalibrationCache(std::size_t& length) noexcept override {
    length = 0;
    return nullptr;
  }

  void writeCalibrationCache(void const* ptr,
                             std::size_t length) noexcept override {
    FILE* fh = std::fopen(cache_out_.c_str(), "wb");
    if (!fh) {
      std::fprintf(stderr, "[calib_generator] cannot write cache: %s\n",
                   cache_out_.c_str());
      return;
    }
    const size_t written = std::fwrite(ptr, 1, length, fh);
    std::fclose(fh);
    if (written != length) {
      std::fprintf(stderr, "[calib_generator] cache write incomplete "
                           "(%zu/%zu bytes)\n", written, length);
    } else {
      cache_bytes_ = length;
    }
  }

  std::size_t batches() const { return batches_; }
  std::size_t cache_bytes() const { return cache_bytes_; }

 private:
  std::vector<std::string> files_;
  int batch_;
  std::string cache_out_;
  std::size_t next_idx_ = 0;
  std::size_t batches_ = 0;
  std::size_t cache_bytes_ = 0;
  void* dev_buf_ = nullptr;
};

std::string requireArg(int argc, char** argv, const char* key) {
  const std::string prefix = std::string("--") + key + "=";
  for (int i = 1; i < argc; ++i) {
    const std::string arg(argv[i]);
    if (arg.rfind(prefix, 0) == 0) return arg.substr(prefix.size());
  }
  throw std::runtime_error(std::string("missing required argument --") + key);
}

int parseOptionalInt(int argc, char** argv, const char* key, int def) {
  try {
    return std::stoi(requireArg(argc, argv, key));
  } catch (const std::runtime_error&) {
    return def;
  }
}

std::string parseOptionalString(int argc, char** argv, const char* key,
                                const char* def) {
  try {
    return requireArg(argc, argv, key);
  } catch (const std::runtime_error&) {
    return def;
  }
}

}  // namespace

int main(int argc, char** argv) {
  try {
    const std::string onnx_path = requireArg(argc, argv, "onnx");
    const std::string cache_out = requireArg(argc, argv, "cache-out");
    const std::string images_dir = requireArg(argc, argv, "images");
    const int batch = parseOptionalInt(argc, argv, "batch", 4);
    const int workspace_mb = parseOptionalInt(argc, argv, "workspace-mb", 2048);
    if (batch <= 0 || workspace_mb <= 0) {
      throw std::runtime_error("batch and workspace-mb must be positive");
    }

    const std::vector<std::string> files =
        jetedge::calib::listRawFiles(images_dir);
    if (files.empty()) {
      throw std::runtime_error("no .rgb frames in " + images_dir);
    }
    std::printf("[calib_generator] frames: %zu, batch: %d, batches: %zu\n",
                files.size(), batch, files.size() / static_cast<size_t>(batch));

    CalibLogger logger;
    std::unique_ptr<nvinfer1::IBuilder> builder(
        nvinfer1::createInferBuilder(logger));
    std::unique_ptr<nvinfer1::INetworkDefinition> network(
        builder->createNetworkV2(0));
    std::unique_ptr<nvonnxparser::IParser> parser(
        nvonnxparser::createParser(*network, logger));
    if (!parser->parseFromFile(onnx_path.c_str(), 1)) {
      throw std::runtime_error("failed to parse ONNX: " + onnx_path);
    }

    nvinfer1::ITensor* input = network->getInput(0);
    if (!input) {
      throw std::runtime_error("network has no input");
    }
    std::printf("[calib_generator] input: %s %s\n", input->getName(),
                std::to_string(input->getDimensions().nbDims).c_str());

    // Optimization profile matching the trtexec build (Stage 5):
    // MIN=1x3x384x640 OPT=4x3x384x640 MAX=4x3x384x640.  The builder retains
    // ownership of the profile (NvInfer.h doc) — no deletion here.
    nvinfer1::IOptimizationProfile* profile = builder->createOptimizationProfile();
    const nvinfer1::Dims4 min_shape(1, 3, 384, 640);
    const nvinfer1::Dims4 opt_shape(4, 3, 384, 640);
    if (!profile->setDimensions(input->getName(),
                                nvinfer1::OptProfileSelector::kMIN,
                                min_shape) ||
        !profile->setDimensions(input->getName(),
                                nvinfer1::OptProfileSelector::kOPT,
                                opt_shape) ||
        !profile->setDimensions(input->getName(),
                                nvinfer1::OptProfileSelector::kMAX,
                                opt_shape)) {
      throw std::runtime_error("failed to set optimization profile dims");
    }

    const std::string algo = parseOptionalString(argc, argv, "algo",
                                                 "entropy2");
    if (algo != "entropy2" && algo != "minmax") {
      throw std::runtime_error("--algo must be 'entropy2' or 'minmax'");
    }
    const std::string save_engine =
        parseOptionalString(argc, argv, "save-engine", "");
    std::printf("[calib_generator] calibration algorithm: %s%s%s\n",
                algo.c_str(), save_engine.empty() ? "" : ", save-engine ",
                save_engine.empty() ? "" : save_engine.c_str());

    // One build per calibrator type (the algorithm is implied by the
    // calibrator base class).  The engine bytes are intentionally
    // discarded (see file comment) — the deliverable engine is built by
    // trtexec --int8 --calib=<cache>.
    auto run = [&](auto* dummy) -> void {
      using CalibBase = std::remove_pointer_t<decltype(dummy)>;
      CalibDataT<CalibBase> calib(files, batch, cache_out);
      std::unique_ptr<nvinfer1::IBuilderConfig> config(
          builder->createBuilderConfig());
      const int32_t profile_index = config->addOptimizationProfile(profile);
      std::printf("[calib_generator] profile index: %d\n",
                  static_cast<int>(profile_index));
      if (profile_index < 0) {
        throw std::runtime_error("addOptimizationProfile failed");
      }
      config->setMemoryPoolLimit(nvinfer1::MemoryPoolType::kWORKSPACE,
                                 static_cast<std::size_t>(workspace_mb) * 1024 *
                                     1024);
      config->setFlag(nvinfer1::BuilderFlag::kFP16);
      config->setFlag(nvinfer1::BuilderFlag::kINT8);
      config->setInt8Calibrator(&calib);  // deprecated since TRT 10.1 but
                                          // the standard PTQ path
      std::printf("[calib_generator] building (calibration inside)...\n");
      const auto t0 = std::chrono::steady_clock::now();
      std::unique_ptr<nvinfer1::IHostMemory> engine(
          builder->buildSerializedNetwork(*network, *config));
      const auto t1 = std::chrono::steady_clock::now();
      if (!engine) {
        throw std::runtime_error("buildSerializedNetwork failed");
      }
      const double secs = std::chrono::duration<double>(t1 - t0).count();
      // Optional engine output: trtexec's --calib only accepts
      // EntropyCalibration2-format caches, so a MinMax-calibrated engine
      // must be produced right here (entropy2 stays on the trtexec path).
      if (!save_engine.empty()) {
        FILE* fh = std::fopen(save_engine.c_str(), "wb");
        if (!fh || std::fwrite(engine->data(), 1, engine->size(), fh) !=
                       engine->size()) {
          if (fh) std::fclose(fh);
          throw std::runtime_error("failed to write engine: " + save_engine);
        }
        std::fclose(fh);
      }
      std::printf("[calib_generator] done: build %.1f s, calibration "
                  "batches %zu, cache %s (%zu bytes), warnings %d%s%s\n",
                  secs, calib.batches(), cache_out.c_str(),
                  calib.cache_bytes(), logger.warnings(),
                  save_engine.empty() ? "" : ", engine ",
                  save_engine.empty() ? "" : save_engine.c_str());
    };

    if (algo == "minmax") {
      run(static_cast<nvinfer1::IInt8MinMaxCalibrator*>(nullptr));
    } else {
      run(static_cast<nvinfer1::IInt8EntropyCalibrator2*>(nullptr));
    }
    return 0;
  } catch (const std::exception& e) {
    std::fprintf(stderr, "[calib_generator] ERROR: %s\n", e.what());
    return 1;
  }
}
