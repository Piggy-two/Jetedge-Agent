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

    // ---- Inference section (optional) -------------------------------------
    if (root["inference"]) {
      const auto& inf_node = root["inference"];
      if (inf_node["enable"])
        config.inference.enable = inf_node["enable"].as<bool>();
      if (inf_node["config_file_path"])
        config.inference.nvinfer_config_path = inf_node["config_file_path"].as<std::string>();
      if (inf_node["gie_unique_id"])
        config.inference.gie_unique_id = inf_node["gie_unique_id"].as<int>();
    }

    // ---- Tracker section (optional) ----------------------------------------
    if (root["tracker"]) {
      const auto& tr_node = root["tracker"];
      if (tr_node["enable"])
        config.tracker.enable = tr_node["enable"].as<bool>();
      if (tr_node["ll_lib_file"])
        config.tracker.ll_lib_file = tr_node["ll_lib_file"].as<std::string>();
      if (tr_node["ll_config_file"])
        config.tracker.ll_config_file = tr_node["ll_config_file"].as<std::string>();
      if (tr_node["width"])
        config.tracker.width = tr_node["width"].as<int>();
      if (tr_node["height"])
        config.tracker.height = tr_node["height"].as<int>();
      if (tr_node["gpu_id"])
        config.tracker.gpu_id = tr_node["gpu_id"].as<int>();
    }

    // ---- Output section (optional) ------------------------------------------
    if (root["output"]) {
      const auto& out_node = root["output"];
      if (out_node["jsonl_path"])
        config.output.jsonl_path = out_node["jsonl_path"].as<std::string>();
      if (out_node["labels_file_path"])
        config.output.labels_file_path = out_node["labels_file_path"].as<std::string>();
      if (out_node["fps_report_interval_sec"])
        config.output.fps_report_interval_sec = out_node["fps_report_interval_sec"].as<int>();
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
