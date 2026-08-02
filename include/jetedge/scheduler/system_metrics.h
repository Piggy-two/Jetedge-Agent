// SystemSampler — read-only Linux system metrics for the Stage 9 scheduler.
//
// Sources (all plain file reads, no sudo, no system changes):
//   CPU  /proc/stat        — aggregate busy% between consecutive calls
//   RAM  /proc/meminfo     — used% of MemTotal
//   Temp /sys/class/thermal/thermal_zone*/temp — max over readable zones
//        (on this Jetson: cpu-thermal, gpu-thermal, soc0-2, tj-thermal;
//         cv0-2 zones are absent and skipped)
//
// A reading that cannot be obtained returns -1; the policy treats missing
// readings as "not pressing" and never lets them block recovery.

#pragma once

#include <string>

#include "jetedge/scheduler/scheduler_policy.h"

namespace jetedge {
namespace scheduler {

class SystemSampler {
 public:
  // CPU busy% since the previous call (first call returns 0).
  double sample_cpu_pct();

  // Used RAM percentage.
  double sample_mem_pct();

  // Max readable thermal-zone temperature in °C (-1 when unavailable).
  double sample_temp_c();

  // Type name of the zone that produced the last sample_temp_c() max
  // (e.g. "gpu-thermal"), or "none".
  const std::string& last_temp_zone() const { return last_temp_zone_; }

  // Full sample in one pass.
  SystemSample sample();

 private:
  uint64_t prev_busy_ = 0;
  uint64_t prev_total_ = 0;
  bool have_prev_ = false;
  std::string last_temp_zone_ = "none";
};

}  // namespace scheduler
}  // namespace jetedge
