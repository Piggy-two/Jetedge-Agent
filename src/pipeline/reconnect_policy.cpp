// ReconnectPolicy implementation — see reconnect_policy.h.

#include "jetedge/pipeline/reconnect_policy.h"

namespace jetedge {
namespace pipeline {

const char* stream_state_str(StreamState s) {
  switch (s) {
    case StreamState::kOffline:      return "OFFLINE";
    case StreamState::kConnecting:   return "CONNECTING";
    case StreamState::kRunning:      return "RUNNING";
    case StreamState::kDegraded:     return "DEGRADED";
    case StreamState::kReconnecting: return "RECONNECTING";
    case StreamState::kFailed:       return "FAILED";
  }
  return "?";
}

ReconnectPolicy::ReconnectPolicy(Params p) : params_(p) {
  // Defensive clamps — config validation happens at load time too.
  if (params_.backoff_base_ms < 100) params_.backoff_base_ms = 100;
  if (params_.backoff_max_ms < params_.backoff_base_ms) {
    params_.backoff_max_ms = params_.backoff_base_ms;
  }
  if (params_.max_consecutive_failures < 1) {
    params_.max_consecutive_failures = 1;
  }
}

void ReconnectPolicy::mark_connect() {
  state_ = StreamState::kConnecting;
  ++attempts_;
}

void ReconnectPolicy::mark_running() {
  state_ = StreamState::kRunning;
  failures_ = 0;
  attempts_ = 0;
  reason_ = "none";
}

void ReconnectPolicy::mark_degraded() {
  if (state_ != StreamState::kRunning) {
    return;  // only degrade from RUNNING
  }
  state_ = StreamState::kDegraded;
  reason_ = "stall";
}

StreamState ReconnectPolicy::mark_failure(const char* reason) {
  ++failures_;
  reason_ = reason ? reason : "unknown";
  if (failures_ > params_.max_consecutive_failures) {
    state_ = StreamState::kFailed;
    return state_;
  }
  state_ = StreamState::kReconnecting;
  ++total_reconnects_;
  return state_;
}

int64_t ReconnectPolicy::backoff_ms() const {
  if (state_ != StreamState::kReconnecting || failures_ < 1) {
    return 0;
  }
  // failures_=1 → base * 2^0 = base; each further failure doubles.
  int64_t shift = static_cast<int64_t>(failures_) - 1;
  if (shift > 60) shift = 60;  // guard against overflow
  const int64_t v = static_cast<int64_t>(params_.backoff_base_ms) << shift;
  return v > params_.backoff_max_ms ? params_.backoff_max_ms : v;
}

}  // namespace pipeline
}  // namespace jetedge
