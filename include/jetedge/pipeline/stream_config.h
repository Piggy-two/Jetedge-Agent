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
  std::string type;       // "file" or "rtsp" (Stage 8)
  std::string uri;        // file path or RTSP URL
  StreamPriority priority = StreamPriority::kNormal;
  double expected_fps = 30.0;  // reserved for later use
};

// RTSP source + reconnect settings (Stage 8).  Global section in the YAML.
struct RtspConfig {
  bool enable = false;             // master switch for RTSP sources
  bool live_source = true;         // nvstreammux live-source (all-RTSP configs)
  int watch_timeout_sec = 5;       // no frames for this long → DEGRADED
  int first_frame_timeout_sec = 12;  // initial wait for the FIRST frame after a
                                     // connect — must exceed the stream's keyframe
                                     // interval (measured GOP on this test env:
                                     // 8.33 s for sample_720p.mp4) or a live stream
                                     // that connects mid-GOP is torn down before its
                                     // first IDR ever arrives
  int max_retries = 5;             // consecutive failures before FAILED
  int backoff_base_ms = 1000;      // first reconnect wait
  int backoff_max_ms = 15000;      // exponential backoff cap
  int verify_sec = 5;              // post-reconnect FPS verification window
  double min_fps = 1.0;            // reconnect success requires this input FPS
  int rtspsrc_latency_ms = 500;    // rtspsrc latency property
  // rtspsrc transport: "tcp" | "udp" | "auto" (rtspsrc default).  TCP avoids
  // UDP socket-buffer overflow on hosts without CAP_NET_ADMIN to raise
  // net.core.rmem_max (measured on this Jetson: 1080p RTP bursts overflow the
  // default 212 KB buffer → packet loss → corrupt decode).
  std::string transport = "auto";
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
