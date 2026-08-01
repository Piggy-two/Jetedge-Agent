// InferenceConfig — nvinfer integration settings (Stage 4).

#pragma once

#include <string>

namespace jetedge {
namespace inference {

struct InferenceConfig {
  bool enable = false;               // insert nvinfer into the pipeline
  std::string nvinfer_config_path;   // path to the nvinfer GIE config file
  int gie_unique_id = 1;             // nvinfer unique-id
};

}  // namespace inference
}  // namespace jetedge
