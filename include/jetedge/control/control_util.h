// Control module shared helpers (header-only).

#pragma once

#include <cstdint>
#include <ctime>

namespace jetedge {
namespace control {

// Monotonic milliseconds (control-rate timing: request ids, throttles).
inline uint64_t mono_ms() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000ULL +
         static_cast<uint64_t>(ts.tv_nsec / 1000000);
}

// Wall-clock milliseconds (snapshot/audit timestamps).
inline uint64_t wall_ms() {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000ULL +
         static_cast<uint64_t>(ts.tv_nsec / 1000000);
}

}  // namespace control
}  // namespace jetedge
