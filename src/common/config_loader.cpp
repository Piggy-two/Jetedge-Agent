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

    // ---- Events section (optional, Stage 6) ---------------------------------
    if (root["events"]) {
      const auto& ev_node = root["events"];
      if (ev_node["enable"])
        config.events.enable = ev_node["enable"].as<bool>();
      if (ev_node["jsonl_path"])
        config.events.jsonl_path = ev_node["jsonl_path"].as<std::string>();
      if (ev_node["keyframe_dir"])
        config.events.keyframe_dir = ev_node["keyframe_dir"].as<std::string>();
      if (ev_node["max_keyframes"]) {
        const int v = ev_node["max_keyframes"].as<int>();
        if (v < 0) { error_out = "events.max_keyframes must be >= 0"; return false; }
        config.events.max_keyframes = v;
      }
      if (ev_node["jpeg_quality"]) {
        const int v = ev_node["jpeg_quality"].as<int>();
        if (v < 1 || v > 100) { error_out = "events.jpeg_quality must be in [1,100]"; return false; }
        config.events.jpeg_quality = v;
      }
      if (ev_node["disappear_grace_frames"]) {
        const int v = ev_node["disappear_grace_frames"].as<int>();
        if (v < 0) { error_out = "events.disappear_grace_frames must be >= 0"; return false; }
        config.events.disappear_grace_frames = static_cast<uint64_t>(v);
      }
      if (ev_node["count_threshold"]) {
        const int v = ev_node["count_threshold"].as<int>();
        if (v < 1) { error_out = "events.count_threshold must be >= 1"; return false; }
        config.events.count_threshold = v;
      }
      if (ev_node["count_hysteresis"]) {
        const int v = ev_node["count_hysteresis"].as<int>();
        if (v < 0 || v >= config.events.count_threshold) {
          error_out = "events.count_hysteresis must be in [0, count_threshold)";
          return false;
        }
        config.events.count_hysteresis = v;
      }
      if (ev_node["classes"]) {
        for (const auto& c : ev_node["classes"]) {
          const int v = c.as<int>();
          if (v < 0) { error_out = "events.classes must contain non-negative ids"; return false; }
          config.events.classes.push_back(v);
        }
      }
      if (ev_node["zones"]) {
        for (const auto& z : ev_node["zones"]) {
          events::ZoneRule zone;
          if (!z["name"]) { error_out = "events.zones[].name is required"; return false; }
          zone.name = z["name"].as<std::string>();
          if (zone.name.empty()) { error_out = "events.zones[].name must not be empty"; return false; }
          if (z["stream_id"]) zone.stream_id = z["stream_id"].as<std::string>();
          if (!z["rect"] || z["rect"].size() != 4) {
            error_out = "events.zones[].rect must be [left, top, width, height]";
            return false;
          }
          zone.left   = z["rect"][0].as<float>();
          zone.top    = z["rect"][1].as<float>();
          zone.width  = z["rect"][2].as<float>();
          zone.height = z["rect"][3].as<float>();
          if (zone.width <= 0 || zone.height <= 0) {
            error_out = "events.zones[].rect width/height must be > 0";
            return false;
          }
          config.events.zones.push_back(std::move(zone));
        }
      }
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
