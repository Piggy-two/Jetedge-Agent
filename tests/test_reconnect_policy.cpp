// Unit tests for ReconnectPolicy (Stage 8 RTSP reconnect state machine).
// Pure logic — no GStreamer dependency.

#include <cstdio>
#include <string>

#include "jetedge/pipeline/reconnect_policy.h"

using jetedge::pipeline::ReconnectPolicy;
using jetedge::pipeline::StreamState;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                     \
  do {                                                                  \
    ++g_checks;                                                         \
    if (!(cond)) {                                                      \
      std::printf("FAIL line %d: %s\n", __LINE__, #cond);               \
      ++g_failures;                                                     \
    }                                                                   \
  } while (0)

static void test_initial_state() {
  ReconnectPolicy p({});
  CHECK(p.state() == StreamState::kOffline);
  CHECK(p.consecutive_failures() == 0);
  CHECK(p.total_reconnects() == 0);
  CHECK(p.backoff_ms() == 0);
}

static void test_connect_running() {
  ReconnectPolicy p({});
  p.mark_connect();
  CHECK(p.state() == StreamState::kConnecting);
  p.mark_running();
  CHECK(p.state() == StreamState::kRunning);
  CHECK(p.consecutive_failures() == 0);
}

static void test_degraded_only_from_running() {
  ReconnectPolicy p({});
  p.mark_degraded();  // OFFLINE — no-op
  CHECK(p.state() == StreamState::kOffline);
  p.mark_connect();
  p.mark_degraded();  // CONNECTING — no-op
  CHECK(p.state() == StreamState::kConnecting);
  p.mark_running();
  p.mark_degraded();  // RUNNING — honored
  CHECK(p.state() == StreamState::kDegraded);
  CHECK(std::string(p.last_reason()) == "stall");
}

static void test_backoff_sequence() {
  // base=1000, max=15000: 1000 → 2000 → 4000 → 8000 → 15000(cap) → FAILED
  ReconnectPolicy::Params pr;
  pr.backoff_base_ms = 1000;
  pr.backoff_max_ms = 15000;
  pr.max_consecutive_failures = 5;
  ReconnectPolicy p(pr);

  const int64_t expected[] = {1000, 2000, 4000, 8000, 15000};
  for (int i = 0; i < 5; ++i) {
    StreamState s = p.mark_failure("conn-lost");
    CHECK(s == StreamState::kReconnecting);
    CHECK(p.backoff_ms() == expected[i]);
    CHECK(p.consecutive_failures() == i + 1);
  }
  // 6th consecutive failure exceeds the budget → FAILED, no more backoff.
  StreamState s = p.mark_failure("conn-lost");
  CHECK(s == StreamState::kFailed);
  CHECK(p.backoff_ms() == 0);
  CHECK(p.total_reconnects() == 5);
}

static void test_failure_reset_after_running() {
  ReconnectPolicy p({.backoff_base_ms = 1000, .backoff_max_ms = 15000, .max_consecutive_failures = 3});
  p.mark_failure("a");
  p.mark_failure("b");
  CHECK(p.consecutive_failures() == 2);
  p.mark_connect();
  p.mark_running();
  CHECK(p.consecutive_failures() == 0);
  CHECK(p.state() == StreamState::kRunning);
  // Fresh budget: 3 more failures allowed before FAILED.
  p.mark_failure("c");
  p.mark_failure("d");
  p.mark_failure("e");
  CHECK(p.state() == StreamState::kReconnecting);
  CHECK(p.mark_failure("f") == StreamState::kFailed);
}

static void test_small_max_cap() {
  // base=1000, max=1500: 1st failure → base (1000), 2nd doubles to 2000 but
  // is capped at max (1500).
  ReconnectPolicy::Params pr;
  pr.backoff_base_ms = 1000;
  pr.backoff_max_ms = 1500;
  ReconnectPolicy p(pr);
  p.mark_failure("x");
  CHECK(p.backoff_ms() == 1000);
  p.mark_failure("y");
  CHECK(p.backoff_ms() == 1500);
}

static void test_reason_recorded() {
  ReconnectPolicy p({});
  p.mark_failure("server-gone");
  CHECK(std::string(p.last_reason()) == "server-gone");
  p.mark_running();
  CHECK(std::string(p.last_reason()) == "none");
}

int main() {
  test_initial_state();
  test_connect_running();
  test_degraded_only_from_running();
  test_backoff_sequence();
  test_failure_reset_after_running();
  test_small_max_cap();
  test_reason_recorded();

  std::printf("reconnect_policy: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
