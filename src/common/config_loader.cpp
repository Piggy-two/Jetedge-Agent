// ConfigLoader — parse streams.yaml using yaml-cpp.

#include "jetedge/common/config_loader.h"

#include <fstream>

#include <yaml-cpp/yaml.h>

#include "jetedge/common/logging.h"

namespace jetedge {
namespace common {

bool load_streams_config(const std::string& path, StreamsConfig& config, std::string& error_out) {
  try {
    YAML::Node root = YAML::LoadFile(path);
    if (!root.IsMap()) {
      error_out = "root node must be a map";
      return false;
    }

    // ---- Mux section -------------------------------------------------------
    if (root["mux"]) {
      const auto& mux_node = root["mux"];
      if (mux_node["output_width"])
        config.mux.output_width = mux_node["output_width"].as<int>();
      if (mux_node["output_height"])
        config.mux.output_height = mux_node["output_height"].as<int>();
      if (mux_node["batch_timeout_usec"])
        config.mux.batch_timeout_usec = mux_node["batch_timeout_usec"].as<int>();
    }

    // ---- Streams section ---------------------------------------------------
    const auto& streams_node = root["streams"];
    if (!streams_node || !streams_node.IsSequence()) {
      error_out = "missing or invalid 'streams' sequence";
      return false;
    }

    for (const auto& s : streams_node) {
      if (!s["id"] || !s["uri"]) {
        error_out = "each stream must have 'id' and 'uri'";
        return false;
      }
      pipeline::StreamConfig sc;
      sc.id   = s["id"].as<std::string>();
      sc.uri  = s["uri"].as<std::string>();
      sc.type = s["type"] ? s["type"].as<std::string>() : "file";
      if (s["priority"])
        sc.priority = pipeline::priority_from_str(s["priority"].as<std::string>());
      if (s["expected_fps"])
        sc.expected_fps = s["expected_fps"].as<double>();
      config.streams.push_back(std::move(sc));
    }

    if (config.streams.empty()) {
      error_out = "at least one stream is required";
      return false;
    }

    return true;
  } catch (const YAML::Exception& e) {
    error_out = std::string("YAML parse error: ") + e.what();
    return false;
  } catch (const std::exception& e) {
    error_out = std::string("config error: ") + e.what();
    return false;
  }
}

}  // namespace common
}  // namespace jetedge
