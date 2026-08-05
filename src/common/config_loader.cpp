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

    // ---- LLM section (optional, Stage 7) -----------------------------------
    if (root["llm"]) {
      const auto& llm_node = root["llm"];
      if (llm_node["enable"])
        config.llm.enable = llm_node["enable"].as<bool>();
      if (llm_node["keyframe_dir"])
        config.llm.keyframe_dir = llm_node["keyframe_dir"].as<std::string>();
      if (llm_node["cloud_output_path"])
        config.llm.cloud_output_path = llm_node["cloud_output_path"].as<std::string>();
      if (llm_node["deepseek_interval_sec"]) {
        const int v = llm_node["deepseek_interval_sec"].as<int>();
        if (v < 0) { error_out = "llm.deepseek_interval_sec must be >= 0"; return false; }
        config.llm.deepseek_interval_sec = v;
      }

      // Provider endpoints.
      if (llm_node["qwen"]) {
        const auto& n = llm_node["qwen"];
        if (n["endpoint"])   config.llm.qwen.endpoint = n["endpoint"].as<std::string>();
        if (n["model"])      config.llm.qwen.model = n["model"].as<std::string>();
        if (n["max_tokens"]) {
          const int v = n["max_tokens"].as<int>();
          if (v < 1 || v > 8192) { error_out = "llm.qwen.max_tokens must be in [1,8192]"; return false; }
          config.llm.qwen.max_tokens = v;
        }
        if (n["timeout_sec"]) {
          const int v = n["timeout_sec"].as<int>();
          if (v < 1 || v > 120) { error_out = "llm.qwen.timeout_sec must be in [1,120]"; return false; }
          config.llm.qwen.timeout_sec = v;
        }
        if (n["max_retries"]) {
          const int v = n["max_retries"].as<int>();
          if (v < 0 || v > 5) { error_out = "llm.qwen.max_retries must be in [0,5]"; return false; }
          config.llm.qwen.max_retries = v;
        }
        if (n["thinking_mode"])
          config.llm.qwen.thinking_mode = n["thinking_mode"].as<bool>();
      }
      if (llm_node["deepseek"]) {
        const auto& n = llm_node["deepseek"];
        if (n["endpoint"])   config.llm.deepseek.endpoint = n["endpoint"].as<std::string>();
        if (n["model"])      config.llm.deepseek.model = n["model"].as<std::string>();
        if (n["max_tokens"]) {
          const int v = n["max_tokens"].as<int>();
          if (v < 1 || v > 8192) { error_out = "llm.deepseek.max_tokens must be in [1,8192]"; return false; }
          config.llm.deepseek.max_tokens = v;
        }
        if (n["timeout_sec"]) {
          const int v = n["timeout_sec"].as<int>();
          if (v < 1 || v > 120) { error_out = "llm.deepseek.timeout_sec must be in [1,120]"; return false; }
          config.llm.deepseek.timeout_sec = v;
        }
        if (n["max_retries"]) {
          const int v = n["max_retries"].as<int>();
          if (v < 0 || v > 5) { error_out = "llm.deepseek.max_retries must be in [0,5]"; return false; }
          config.llm.deepseek.max_retries = v;
        }
        if (n["thinking_mode"])
          config.llm.deepseek.thinking_mode = n["thinking_mode"].as<bool>();
      }

      // Queue settings.
      if (llm_node["queue"]) {
        const auto& n = llm_node["queue"];
        if (n["max_size"]) {
          const int v = n["max_size"].as<int>();
          if (v < 1 || v > 256) { error_out = "llm.queue.max_size must be in [1,256]"; return false; }
          config.llm.queue.max_size = v;
        }
        if (n["worker_threads"]) {
          const int v = n["worker_threads"].as<int>();
          if (v < 1 || v > 8) { error_out = "llm.queue.worker_threads must be in [1,8]"; return false; }
          config.llm.queue.worker_threads = v;
        }
      }

      // Circuit breaker settings.
      if (llm_node["circuit_breaker"]) {
        const auto& n = llm_node["circuit_breaker"];
        if (n["failure_threshold"]) {
          const int v = n["failure_threshold"].as<int>();
          if (v < 1) { error_out = "llm.circuit_breaker.failure_threshold must be >= 1"; return false; }
          config.llm.circuit_breaker.failure_threshold = v;
        }
        if (n["reset_timeout_sec"]) {
          const int v = n["reset_timeout_sec"].as<int>();
          if (v < 1) { error_out = "llm.circuit_breaker.reset_timeout_sec must be >= 1"; return false; }
          config.llm.circuit_breaker.reset_timeout_sec = v;
        }
        if (n["half_open_success_threshold"]) {
          const int v = n["half_open_success_threshold"].as<int>();
          if (v < 1) { error_out = "llm.circuit_breaker.half_open_success_threshold must be >= 1"; return false; }
          config.llm.circuit_breaker.half_open_success_threshold = v;
        }
      }

      // Routing table.
      if (llm_node["routing"]) {
        const auto& n = llm_node["routing"];
        if (n["appearance_to_qwen"])
          config.llm.routing.appearance_to_qwen = n["appearance_to_qwen"].as<bool>();
        if (n["disappearance_to_qwen"])
          config.llm.routing.disappearance_to_qwen = n["disappearance_to_qwen"].as<bool>();
        if (n["zone_entry_to_qwen"])
          config.llm.routing.zone_entry_to_qwen = n["zone_entry_to_qwen"].as<bool>();
        if (n["count_high_to_qwen"])
          config.llm.routing.count_high_to_qwen = n["count_high_to_qwen"].as<bool>();
      }
    }

    // ---- RTSP section (optional, Stage 8) ----------------------------------
    if (root["rtsp"]) {
      const auto& r_node = root["rtsp"];
      if (r_node["enable"])
        config.rtsp.enable = r_node["enable"].as<bool>();
      if (r_node["live_source"])
        config.rtsp.live_source = r_node["live_source"].as<bool>();
      if (r_node["watch_timeout_sec"]) {
        const int v = r_node["watch_timeout_sec"].as<int>();
        if (v < 1 || v > 60) { error_out = "rtsp.watch_timeout_sec must be in [1,60]"; return false; }
        config.rtsp.watch_timeout_sec = v;
      }
      if (r_node["first_frame_timeout_sec"]) {
        const int v = r_node["first_frame_timeout_sec"].as<int>();
        if (v < 1 || v > 120) { error_out = "rtsp.first_frame_timeout_sec must be in [1,120]"; return false; }
        config.rtsp.first_frame_timeout_sec = v;
      }
      if (r_node["max_retries"]) {
        const int v = r_node["max_retries"].as<int>();
        if (v < 1 || v > 20) { error_out = "rtsp.max_retries must be in [1,20]"; return false; }
        config.rtsp.max_retries = v;
      }
      if (r_node["backoff_base_ms"]) {
        const int v = r_node["backoff_base_ms"].as<int>();
        if (v < 100 || v > 60000) { error_out = "rtsp.backoff_base_ms must be in [100,60000]"; return false; }
        config.rtsp.backoff_base_ms = v;
      }
      if (r_node["backoff_max_ms"]) {
        const int v = r_node["backoff_max_ms"].as<int>();
        if (v < 100 || v > 600000) { error_out = "rtsp.backoff_max_ms must be in [100,600000]"; return false; }
        config.rtsp.backoff_max_ms = v;
      }
      if (config.rtsp.backoff_max_ms < config.rtsp.backoff_base_ms) {
        error_out = "rtsp.backoff_max_ms must be >= rtsp.backoff_base_ms";
        return false;
      }
      if (r_node["verify_sec"]) {
        const int v = r_node["verify_sec"].as<int>();
        if (v < 1 || v > 60) { error_out = "rtsp.verify_sec must be in [1,60]"; return false; }
        config.rtsp.verify_sec = v;
      }
      if (r_node["min_fps"]) {
        const double v = r_node["min_fps"].as<double>();
        if (v <= 0.0 || v > 60.0) { error_out = "rtsp.min_fps must be in (0,60]"; return false; }
        config.rtsp.min_fps = v;
      }
      if (r_node["rtspsrc_latency_ms"]) {
        const int v = r_node["rtspsrc_latency_ms"].as<int>();
        if (v < 0 || v > 10000) { error_out = "rtsp.rtspsrc_latency_ms must be in [0,10000]"; return false; }
        config.rtsp.rtspsrc_latency_ms = v;
      }
      if (r_node["transport"]) {
        const std::string t = r_node["transport"].as<std::string>();
        if (t != "tcp" && t != "udp" && t != "auto") {
          error_out = "rtsp.transport must be 'tcp', 'udp' or 'auto'";
          return false;
        }
        config.rtsp.transport = t;
      }
    }

    // ---- Scheduler section (optional, Stage 9) ------------------------------
    if (root["scheduler"]) {
      const auto& s_node = root["scheduler"];
      if (s_node["enable"])
        config.scheduler.enable = s_node["enable"].as<bool>();
      if (s_node["sample_interval_sec"]) {
        const int v = s_node["sample_interval_sec"].as<int>();
        if (v < 1 || v > 60) { error_out = "scheduler.sample_interval_sec must be in [1,60]"; return false; }
        config.scheduler.sample_interval_sec = v;
      }
      if (s_node["pressure_cpu_enter"]) {
        const double v = s_node["pressure_cpu_enter"].as<double>();
        if (v <= 0.0 || v >= 100.0) { error_out = "scheduler.pressure_cpu_enter must be in (0,100)"; return false; }
        config.scheduler.pressure_cpu_enter = v;
      }
      if (s_node["pressure_cpu_exit"]) {
        const double v = s_node["pressure_cpu_exit"].as<double>();
        if (v <= 0.0 || v >= 100.0) { error_out = "scheduler.pressure_cpu_exit must be in (0,100)"; return false; }
        config.scheduler.pressure_cpu_exit = v;
      }
      if (config.scheduler.pressure_cpu_exit >= config.scheduler.pressure_cpu_enter) {
        error_out = "scheduler.pressure_cpu_exit must be < scheduler.pressure_cpu_enter";
        return false;
      }
      if (s_node["thermal_temp_enter"]) {
        const double v = s_node["thermal_temp_enter"].as<double>();
        if (v < 30.0 || v > 125.0) { error_out = "scheduler.thermal_temp_enter must be in [30,125]"; return false; }
        config.scheduler.thermal_temp_enter = v;
      }
      if (s_node["thermal_temp_exit"]) {
        const double v = s_node["thermal_temp_exit"].as<double>();
        if (v < 30.0 || v > 125.0) { error_out = "scheduler.thermal_temp_exit must be in [30,125]"; return false; }
        config.scheduler.thermal_temp_exit = v;
      }
      if (config.scheduler.thermal_temp_exit >= config.scheduler.thermal_temp_enter) {
        error_out = "scheduler.thermal_temp_exit must be < scheduler.thermal_temp_enter";
        return false;
      }
      if (s_node["critical_temp_enter"]) {
        const double v = s_node["critical_temp_enter"].as<double>();
        if (v < 30.0 || v > 125.0) { error_out = "scheduler.critical_temp_enter must be in [30,125]"; return false; }
        config.scheduler.critical_temp_enter = v;
      }
      if (s_node["critical_temp_exit"]) {
        const double v = s_node["critical_temp_exit"].as<double>();
        if (v < 30.0 || v > 125.0) { error_out = "scheduler.critical_temp_exit must be in [30,125]"; return false; }
        config.scheduler.critical_temp_exit = v;
      }
      if (config.scheduler.critical_temp_exit >= config.scheduler.critical_temp_enter) {
        error_out = "scheduler.critical_temp_exit must be < scheduler.critical_temp_enter";
        return false;
      }
      if (config.scheduler.critical_temp_enter <= config.scheduler.thermal_temp_enter) {
        error_out = "scheduler.critical_temp_enter must be > scheduler.thermal_temp_enter";
        return false;
      }
      if (s_node["min_hold_sec"]) {
        const int v = s_node["min_hold_sec"].as<int>();
        if (v < 0 || v > 600) { error_out = "scheduler.min_hold_sec must be in [0,600]"; return false; }
        config.scheduler.min_hold_ms = static_cast<uint64_t>(v) * 1000;
      }
      if (s_node["cooldown_sec"]) {
        const int v = s_node["cooldown_sec"].as<int>();
        if (v < 0 || v > 600) { error_out = "scheduler.cooldown_sec must be in [0,600]"; return false; }
        config.scheduler.cooldown_ms = static_cast<uint64_t>(v) * 1000;
      }
      if (s_node["max_adjustments_per_window"]) {
        const int v = s_node["max_adjustments_per_window"].as<int>();
        if (v < 0 || v > 10) { error_out = "scheduler.max_adjustments_per_window must be in [0,10]"; return false; }
        config.scheduler.max_adjustments_per_window = v;
      }
      if (s_node["adjust_window_sec"]) {
        const int v = s_node["adjust_window_sec"].as<int>();
        if (v < 10 || v > 3600) { error_out = "scheduler.adjust_window_sec must be in [10,3600]"; return false; }
        config.scheduler.adjust_window_ms = static_cast<uint64_t>(v) * 1000;
      }
    }

    // ---- Control section (optional, Stage 11) ------------------------------
    if (root["control"]) {
      const auto& c_node = root["control"];
      if (c_node["enable"])
        config.control.enable = c_node["enable"].as<bool>();
      if (c_node["host"])
        config.control.host = c_node["host"].as<std::string>();
      if (c_node["port"]) {
        const int v = c_node["port"].as<int>();
        if (v < 1 || v > 65535) { error_out = "control.port must be in [1,65535]"; return false; }
        config.control.port = v;
      }
      if (c_node["state_dir"]) {
        const std::string v = c_node["state_dir"].as<std::string>();
        if (v.empty()) { error_out = "control.state_dir must not be empty"; return false; }
        config.control.state_dir = v;
      }
      if (c_node["max_body_bytes"]) {
        const int v = c_node["max_body_bytes"].as<int>();
        if (v < 256 || v > 1048576) { error_out = "control.max_body_bytes must be in [256,1048576]"; return false; }
        config.control.max_body_bytes = static_cast<size_t>(v);
      }
      if (c_node["read_timeout_ms"]) {
        const int v = c_node["read_timeout_ms"].as<int>();
        if (v < 100 || v > 60000) { error_out = "control.read_timeout_ms must be in [100,60000]"; return false; }
        config.control.read_timeout_ms = v;
      }
      if (c_node["max_infer_interval"]) {
        const int v = c_node["max_infer_interval"].as<int>();
        if (v < 1 || v > 60) { error_out = "control.max_infer_interval must be in [1,60]"; return false; }
        config.control.max_infer_interval = v;
      }
      if (c_node["restart_min_interval_ms"]) {
        const int v = c_node["restart_min_interval_ms"].as<int>();
        if (v < 0 || v > 600000) { error_out = "control.restart_min_interval_ms must be in [0,600000]"; return false; }
        config.control.restart_min_interval_ms = v;
      }
      if (c_node["max_snapshots"]) {
        const int v = c_node["max_snapshots"].as<int>();
        if (v < 1 || v > 1000) { error_out = "control.max_snapshots must be in [1,1000]"; return false; }
        config.control.max_snapshots = v;
      }
      if (c_node["benchmark_min_duration_s"]) {
        const int v = c_node["benchmark_min_duration_s"].as<int>();
        if (v < 1 || v > 3600) { error_out = "control.benchmark_min_duration_s must be in [1,3600]"; return false; }
        config.control.benchmark_min_duration_s = v;
      }
      if (c_node["benchmark_max_duration_s"]) {
        const int v = c_node["benchmark_max_duration_s"].as<int>();
        if (v < 1 || v > 3600) { error_out = "control.benchmark_max_duration_s must be in [1,3600]"; return false; }
        config.control.benchmark_max_duration_s = v;
      }
      if (c_node["benchmark_default_duration_s"]) {
        const int v = c_node["benchmark_default_duration_s"].as<int>();
        if (v < 1 || v > 3600) { error_out = "control.benchmark_default_duration_s must be in [1,3600]"; return false; }
        config.control.benchmark_default_duration_s = v;
      }
      if (c_node["cors"]) {
        config.control.cors = c_node["cors"].as<bool>();
      }
      if (c_node["dashboard_file"]) {
        const std::string v = c_node["dashboard_file"].as<std::string>();
        if (v.empty()) { error_out = "control.dashboard_file must not be empty"; return false; }
        config.control.dashboard_file = v;
      }
      if (config.control.benchmark_min_duration_s >=
          config.control.benchmark_max_duration_s) {
        error_out = "control.benchmark_min_duration_s must be < benchmark_max_duration_s";
        return false;
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
      if (sc.type != "file" && sc.type != "rtsp") {
        error_out = "streams[].type must be 'file' or 'rtsp'";
        return false;
      }
      if (sc.type == "rtsp" && !config.rtsp.enable) {
        error_out = "streams[].type 'rtsp' requires the 'rtsp.enable: true' section";
        return false;
      }
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

    // Stage 15 P1: hand the event/keyframe output locations to the Control
    // API (read-only display endpoints) — parsed after both sections above.
    config.control.events_jsonl_path = config.events.jsonl_path;
    config.control.keyframes_dir = config.events.keyframe_dir;

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
