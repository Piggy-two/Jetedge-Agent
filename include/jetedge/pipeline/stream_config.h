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

}  // namespace pipeline
}  // namespace jetedge
