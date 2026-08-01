// Unit tests for the Stage 7 CircuitBreaker.
//
// Exercises: CLOSED → OPEN on threshold; OPEN rejects while timing out;
// OPEN → HALF_OPEN after the recovery timeout; HALF_OPEN probe budget;
// HALF_OPEN → CLOSED on consecutive successes; HALF_OPEN → OPEN on probe
// failure; failure counter reset on success.

#include <cstdio>
#include <chrono>
#include <thread>

#include "jetedge/llm/circuit_breaker.h"

using jetedge::llm::CircuitBreaker;
using jetedge::llm::CircuitBreakerConfig;
using jetedge::llm::CircuitState;

namespace {

int g_failures = 0;

void check(bool cond, const char* what) {
  if (cond) {
    std::printf("  PASS  %s\n", what);
  } else {
    std::printf("  FAIL  %s\n", what);
    ++g_failures;
  }
}

}  // namespace

int main() {
  // ---- 1. CLOSED → OPEN after failure_threshold consecutive failures ----
  {
    CircuitBreakerConfig cfg;
    cfg.failure_threshold = 3;
    cfg.reset_timeout_sec = 60;
    cfg.half_open_success_threshold = 2;
    CircuitBreaker cb(cfg, "test");

    check(cb.state() == CircuitState::kClosed, "1a: initial state CLOSED");
    check(cb.allow_request(), "1b: request allowed while CLOSED");
    cb.record_failure();
    cb.record_failure();
    check(cb.state() == CircuitState::kClosed, "1c: still CLOSED below threshold");
    cb.record_failure();
    check(cb.state() == CircuitState::kOpen, "1d: OPEN after threshold failures");
    check(cb.consecutive_failures() == 0, "1e: failure counter reset on OPEN");
    check(!cb.allow_request(), "1f: requests rejected while OPEN");
  }

  // ---- 2. OPEN → HALF_OPEN after the recovery timeout ----
  {
    CircuitBreakerConfig cfg;
    cfg.failure_threshold = 1;
    cfg.reset_timeout_sec = 1;
    cfg.half_open_success_threshold = 2;
    CircuitBreaker cb(cfg, "test");

    cb.record_failure();
    check(cb.state() == CircuitState::kOpen, "2a: OPEN immediately");
    check(!cb.allow_request(), "2b: rejected before timeout");
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    check(cb.allow_request(), "2c: HALF_OPEN probe allowed after timeout");
    check(cb.state() == CircuitState::kHalfOpen, "2d: state is HALF_OPEN");
  }

  // ---- 3. HALF_OPEN probe budget ----
  {
    CircuitBreakerConfig cfg;
    cfg.failure_threshold = 1;
    cfg.reset_timeout_sec = 1;
    cfg.half_open_success_threshold = 2;
    CircuitBreaker cb(cfg, "test");

    cb.record_failure();
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    check(cb.allow_request(), "3a: probe 1 allowed");
    check(cb.allow_request(), "3b: probe 2 allowed");
    check(!cb.allow_request(), "3c: 3rd probe rejected (budget used)");
  }

  // ---- 4. HALF_OPEN → CLOSED on consecutive successes ----
  {
    CircuitBreakerConfig cfg;
    cfg.failure_threshold = 1;
    cfg.reset_timeout_sec = 1;
    cfg.half_open_success_threshold = 2;
    CircuitBreaker cb(cfg, "test");

    cb.record_failure();
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    check(cb.allow_request(), "4a: probe 1");
    cb.record_success();
    check(cb.state() == CircuitState::kHalfOpen, "4b: still HALF_OPEN after 1 success");
    check(cb.allow_request(), "4c: probe 2");
    cb.record_success();
    check(cb.state() == CircuitState::kClosed, "4d: CLOSED after threshold successes");
    check(cb.allow_request(), "4e: requests flow again");
  }

  // ---- 5. HALF_OPEN → OPEN on probe failure ----
  {
    CircuitBreakerConfig cfg;
    cfg.failure_threshold = 5;   // high — probe failure must re-open immediately
    cfg.reset_timeout_sec = 1;
    cfg.half_open_success_threshold = 1;
    CircuitBreaker cb(cfg, "test");

    cb.record_failure();  // 1 < 5, still CLOSED... need 5
    cb.record_failure();
    cb.record_failure();
    cb.record_failure();
    cb.record_failure();
    check(cb.state() == CircuitState::kOpen, "5a: OPEN after 5 failures");
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    check(cb.allow_request(), "5b: HALF_OPEN probe");
    cb.record_failure();  // probe fails
    check(cb.state() == CircuitState::kOpen, "5c: re-opened on probe failure");
    check(!cb.allow_request(), "5d: rejected again");
  }

  // ---- 6. Success in CLOSED resets the failure counter ----
  {
    CircuitBreakerConfig cfg;
    cfg.failure_threshold = 5;
    cfg.reset_timeout_sec = 60;
    CircuitBreaker cb(cfg, "test");

    cb.record_failure();
    cb.record_failure();
    cb.record_failure();
    cb.record_success();   // resets the counter
    cb.record_failure();
    cb.record_failure();
    cb.record_failure();
    cb.record_failure();
    check(cb.state() == CircuitState::kClosed, "6a: still CLOSED below threshold");
    cb.record_failure();   // 5th failure after the reset
    check(cb.state() == CircuitState::kOpen, "6b: OPEN after failures post-reset");
  }

  std::printf("\n%s (%d failure(s))\n", g_failures == 0 ? "ALL PASS" : "FAILURES",
              g_failures);
  return g_failures == 0 ? 0 : 1;
}
