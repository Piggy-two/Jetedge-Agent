// ConfigLoader — parse streams.yaml into StreamConfig + MuxConfig.
//
// Uses libyaml-cpp (system-installed, /usr/include/yaml-cpp).

#pragma once

#include <string>
#include <vector>

#include "jetedge/pipeline/stream_config.h"

namespace jetedge {
namespace common {

struct StreamsConfig {
  pipeline::MuxConfig mux;
  std::vector<pipeline::StreamConfig> streams;
};

// Parse a YAML file.  Returns true on success.
// On failure, writes an error message to `error_out` and returns false.
bool load_streams_config(const std::string& path, StreamsConfig& config, std::string& error_out);

}  // namespace common
}  // namespace jetedge
