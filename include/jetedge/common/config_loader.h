// ConfigLoader — parse streams.yaml into StreamConfig + MuxConfig.
//
// Uses libyaml-cpp (system-installed, /usr/include/yaml-cpp).

#pragma once

#include <string>
#include <vector>

#include "jetedge/events/event_types.h"
#include "jetedge/inference/inference_config.h"
#include "jetedge/llm/llm_config.h"
#include "jetedge/pipeline/stream_config.h"

namespace jetedge {
namespace common {

struct StreamsConfig {
  pipeline::MuxConfig mux;
  inference::InferenceConfig inference;
  pipeline::TrackerConfig tracker;
  pipeline::OutputConfig output;
  events::EventsConfig events;
  llm::LlmConfig llm;                 // Stage 7: async cloud analysis
  std::vector<pipeline::StreamConfig> streams;
};

// Parse a YAML file.  Returns true on success.
// On failure, writes an error message to `error_out` and returns false.
bool load_streams_config(const std::string& path, StreamsConfig& config, std::string& error_out);

}  // namespace common
}  // namespace jetedge
