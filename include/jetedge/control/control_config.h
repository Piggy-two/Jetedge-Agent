// ControlConfig — safe Control API server settings (Stage 11).
//
// Parsed from the optional `control` YAML group.  Disabled by default: the
// Control API is inert unless explicitly enabled.  All defaults are
// conservative (loopback-only, small body limit, short read timeout).

#pragma once

#include <cstddef>
#include <string>

namespace jetedge {
namespace control {

struct ControlConfig {
  bool enable = false;
  std::string host = "127.0.0.1";  // loopback only by default
  int port = 8080;

  // Directory for config snapshots and the audit log.  Runtime data, must
  // never be committed to Git.
  std::string state_dir = "logs/control";

  // HTTP request limits.
  size_t max_body_bytes = 4096;   // reject larger JSON bodies
  int read_timeout_ms = 5000;     // per-connection read timeout

  // Write-op policy (implementation_plan §59).
  int max_infer_interval = 5;        // infer_interval must be in [0,5]
  int restart_min_interval_ms = 30000;  // per-stream restart throttle
  int max_snapshots = 32;            // keep at most this many snapshot files

  // POST /benchmark measurement window (Stage 12).
  int benchmark_min_duration_s = 5;     // floor for duration_s
  int benchmark_max_duration_s = 120;   // ceiling for duration_s
  int benchmark_default_duration_s = 60;
};

}  // namespace control
}  // namespace jetedge
