// CircuitBreaker — per-provider failure guard (Stage 7).
//
// States: CLOSED (normal) → OPEN (after failure_threshold consecutive
// failures, rejects all requests) → HALF_OPEN (after reset_timeout_sec,
// allows a small number of probe requests) → CLOSED (on successes) or
// back to OPEN (on any probe failure).
//
// Thread-safe: called from the worker thread and, potentially, the
// periodic report path.  The real-time pipeline never calls into this
// class on its hot path (routing happens in the probe, but the circuit
// is only stepped by the worker).

#pragma once

#include <atomic>
#include <chrono>
#include <mutex>
#include <string>

#include "jetedge/llm/llm_config.h"

namespace jetedge {
namespace llm {

enum class CircuitState { kClosed, kOpen, kHalfOpen };

inline const char* circuit_state_str(CircuitState s) {
  switch (s) {
    case CircuitState::kClosed:   return "closed";
    case CircuitState::kOpen:     return "open";
    case CircuitState::kHalfOpen: return "half_open";
  }
  return "unknown";
}

class CircuitBreaker {
 public:
  CircuitBreaker(CircuitBreakerConfig config, const std::string& name);

  // True if a request may proceed (CLOSED, or HALF_OPEN with probe
  // budget left).  False when OPEN — the caller drops the request.
  bool allow_request();

  // Record an attempt outcome (must only be called when allow_request()
  // returned true).  May transition state.
  void record_success();
  void record_failure();

  CircuitState state() const;
  int consecutive_failures() const;

 private:
  void transition_to_open();
  void transition_to_half_open();
  void transition_to_closed();

  CircuitBreakerConfig config_;
  std::string name_;

  mutable std::mutex mu_;
  CircuitState state_ = CircuitState::kClosed;
  int failures_ = 0;                 // consecutive failures while CLOSED
  int successes_ = 0;                // consecutive successes while HALF_OPEN
  int half_open_used_ = 0;           // probe requests consumed in HALF_OPEN
  std::chrono::steady_clock::time_point opened_at_{};
};

}  // namespace llm
}  // namespace jetedge
