// StreamConfig — per-stream configuration parsed from YAML.

#pragma once

#include <string>

namespace jetedge {
namespace pipeline {

enum class StreamPriority { kHigh = 0, kNormal = 1, kLow = 2 };

inline const char* priority_str(StreamPriority p) {
  switch (p) {
    case StreamPriority::kHigh:   return "high";
    case StreamPriority::kNormal: return "normal";
    case StreamPriority::kLow:    return "low";
  }
  return "???";
}

inline StreamPriority priority_from_str(const std::string& s) {
  if (s == "high")   return StreamPriority::kHigh;
  if (s == "low")    return StreamPriority::kLow;
  return StreamPriority::kNormal;  // default
}

struct StreamConfig {
  std::string id;         // e.g. "cam1"
  std::string type;       // "file" (reserved: "rtsp")
  std::string uri;        // file path or RTSP URL
  StreamPriority priority = StreamPriority::kNormal;
  double expected_fps = 30.0;  // reserved for later use
};

struct MuxConfig {
  int output_width = 1920;
  int output_height = 1080;
  int batch_timeout_usec = 40000;  // -1 = wait infinitely
};

// nvtracker integration settings (Stage 5).
struct TrackerConfig {
  bool enable = false;
  std::string ll_lib_file;     // low-level tracker library (absolute path)
  std::string ll_config_file;  // low-level tracker YAML (absolute path)
  int width = 960;             // must be a multiple of 32
  int height = 544;            // must be a multiple of 32
  int gpu_id = 0;
};

// Structured output settings (Stage 5).
struct OutputConfig {
  std::string jsonl_path;             // per-detection JSONL output file
  std::string labels_file_path;       // coco labelfile (id → class name)
  int fps_report_interval_sec = 5;    // periodic FPS logging interval
};

}  // namespace pipeline
}  // namespace jetedge
