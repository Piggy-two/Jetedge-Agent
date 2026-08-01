// CircuitBreaker implementation — see circuit_breaker.h.

#include "jetedge/llm/circuit_breaker.h"

#include <chrono>

#include "jetedge/common/logging.h"

namespace jetedge {
namespace llm {

CircuitBreaker::CircuitBreaker(CircuitBreakerConfig config, const std::string& name)
    : config_(config), name_(name) {}

bool CircuitBreaker::allow_request() {
  std::lock_guard<std::mutex> lock(mu_);
  switch (state_) {
    case CircuitState::kClosed:
      return true;
    case CircuitState::kOpen: {
      // Transition OPEN → HALF_OPEN once the recovery timeout elapsed.
      const auto now = std::chrono::steady_clock::now();
      const auto elapsed =
          std::chrono::duration_cast<std::chrono::seconds>(now - opened_at_)
              .count();
      if (elapsed >= config_.reset_timeout_sec) {
        transition_to_half_open();
        half_open_used_ = 1;
        return true;
      }
      return false;
    }
    case CircuitState::kHalfOpen:
      // Allow only a bounded number of probe requests.
      if (half_open_used_ < config_.half_open_success_threshold) {
        ++half_open_used_;
        return true;
      }
      return false;
  }
  return false;
}

void CircuitBreaker::record_success() {
  std::lock_guard<std::mutex> lock(mu_);
  if (state_ == CircuitState::kHalfOpen) {
    ++successes_;
    if (successes_ >= config_.half_open_success_threshold) {
      transition_to_closed();
      LOG_INFO("llm", "circuit breaker %s: HALF_OPEN → CLOSED (%d consecutive success)",
               name_.c_str(), successes_);
    }
    return;
  }
  // CLOSED success resets the failure counter.
  failures_ = 0;
}

void CircuitBreaker::record_failure() {
  std::lock_guard<std::mutex> lock(mu_);
  if (state_ == CircuitState::kHalfOpen) {
    // A probe failure re-opens the circuit immediately.
    transition_to_open();
    LOG_WARN("llm", "circuit breaker %s: HALF_OPEN probe failed → OPEN", name_.c_str());
    return;
  }
  ++failures_;
  if (failures_ >= config_.failure_threshold) {
    const int reached = failures_;
    transition_to_open();
    LOG_WARN("llm", "circuit breaker %s: CLOSED → OPEN (%d consecutive failures)",
             name_.c_str(), reached);
  }
}

CircuitState CircuitBreaker::state() const {
  std::lock_guard<std::mutex> lock(mu_);
  return state_;
}

int CircuitBreaker::consecutive_failures() const {
  std::lock_guard<std::mutex> lock(mu_);
  return failures_;
}

void CircuitBreaker::transition_to_open() {
  state_ = CircuitState::kOpen;
  failures_ = 0;
  successes_ = 0;
  half_open_used_ = 0;
  opened_at_ = std::chrono::steady_clock::now();
}

void CircuitBreaker::transition_to_half_open() {
  state_ = CircuitState::kHalfOpen;
  successes_ = 0;
  half_open_used_ = 0;
}

void CircuitBreaker::transition_to_closed() {
  state_ = CircuitState::kClosed;
  failures_ = 0;
  successes_ = 0;
  half_open_used_ = 0;
}

}  // namespace llm
}  // namespace jetedge
