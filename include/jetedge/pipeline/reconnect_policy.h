// ReconnectPolicy — deterministic per-stream RTSP reconnect state machine.
//
// Pure logic, no GStreamer dependency (unit-testable in isolation).
// State flow (CLAUDE.md §15):
//
//   OFFLINE → CONNECTING → RUNNING → DEGRADED → RECONNECTING → FAILED
//                 ↑            ↑          └──────────┘
//                 └──── RECONNECTING ────┘
//
// Backoff is exponential (base * 2^n) capped at backoff_max_ms.  After
// max_consecutive_failures the source enters FAILED and automatic retries
// stop (no retry storms).  A successful connect resets the failure counter.

#pragma once

#include <cstdint>

namespace jetedge {
namespace pipeline {

// Per-stream RTSP source lifecycle state.
enum class StreamState {
  kOffline,       // initial, not yet started
  kConnecting,    // connect attempt in progress
  kRunning,       // frames flowing normally
  kDegraded,      // frames stalled / watchdog fired, recovery pending
  kReconnecting,  // waiting for backoff before the next attempt
  kFailed,        // retry budget exhausted — no more automatic attempts
};

const char* stream_state_str(StreamState s);

class ReconnectPolicy {
 public:
  struct Params {
    int backoff_base_ms = 1000;          // first reconnect wait
    int backoff_max_ms = 15000;          // exponential backoff cap
    int max_consecutive_failures = 5;    // FAILED threshold
  };

  explicit ReconnectPolicy(Params p);

  // Current state.
  StreamState state() const { return state_; }

  // A connect attempt started (initial or after reconnect).
  void mark_connect();

  // Frames flowing again (or post-reconnect FPS verification passed).
  void mark_running();

  // Frames stalled (watchdog fired).  Only honored while RUNNING.
  void mark_degraded();

  // The current attempt failed.  Transitions to kReconnecting (wait
  // backoff_ms()) or kFailed once the retry budget is exhausted.
  StreamState mark_failure(const char* reason);

  // Milliseconds to wait before the next reconnect attempt (0 when FAILED).
  int64_t backoff_ms() const;

  // Consecutive failures since the last success (0..max_consecutive_failures).
  int consecutive_failures() const { return failures_; }

  // Connect attempts since the last success.
  int attempts_since_success() const { return attempts_; }

  // Total reconnects since start (never reset — for metrics).
  int64_t total_reconnects() const { return total_reconnects_; }

  // Last recorded failure reason (short string).
  const char* last_reason() const { return reason_; }

 private:
  Params params_;
  StreamState state_ = StreamState::kOffline;
  int failures_ = 0;          // consecutive failures since last success
  int attempts_ = 0;          // connect attempts since last success
  int64_t total_reconnects_ = 0;
  const char* reason_ = "none";
};

}  // namespace pipeline
}  // namespace jetedge
