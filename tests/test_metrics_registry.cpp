// Unit tests for the Stage 12 latency tracking in MetricsRegistry.
// A injected clock makes every percentile/pairing rule deterministic.
// Pure logic — no GStreamer dependency.

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <thread>
#include <vector>

#include "jetedge/metrics/metrics_registry.h"

using jetedge::metrics::MetricsRegistry;

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

#define CHECK_EQ(a, b)                                                  \
  do {                                                                  \
    ++g_checks;                                                         \
    if (!((a) == (b))) {                                                \
      std::printf("FAIL line %d: %s == %s (%s vs %s)\n", __LINE__,      \
                  #a, #b, (std::ostringstream() << (a)).str().c_str(),  \
                  (std::ostringstream() << (b)).str().c_str());         \
      ++g_failures;                                                     \
    }                                                                   \
  } while (0)

namespace {

// Deterministic fake clock: ns, advanced manually.  Atomic so the stress test
// can advance it from one thread while the registry reads it from another.
struct FakeClock {
  std::atomic<uint64_t> now{0};
  uint64_t operator()() { return now.load(); }
  void advance_ms(uint64_t ms) { now.fetch_add(ms * 1000000ULL); }
  void advance_us(uint64_t us) { now.fetch_add(us * 1000ULL); }
};

// 5 in-flight frames: begin at t=0, end at t=10/20/30/40/50 ms.
void add_five_samples(MetricsRegistry* reg, int idx, FakeClock* clock,
                      uint64_t first_frame = 1) {
  for (int i = 0; i < 5; ++i) {
    reg->on_latency_begin(idx, first_frame + static_cast<uint64_t>(i));
  }
  for (int i = 0; i < 5; ++i) {
    clock->advance_ms(10);
    reg->on_latency_end(idx, first_frame + static_cast<uint64_t>(i));
  }
}

}  // namespace

static void test_exact_percentiles() {
  FakeClock clock;
  MetricsRegistry reg([&clock]() { return clock.now.load(); });
  const int idx = reg.register_stream("cam1");
  add_five_samples(&reg, idx, &clock);  // latencies 10,20,30,40,50 ms

  const auto stats = reg.latency_stats(idx);
  CHECK_EQ(stats.samples, 5U);
  CHECK_EQ(stats.avg_ms, 30.0);
  CHECK_EQ(stats.p50_ms, 30.0);  // ceil(5*0.5)-1 = 2 → 30
  CHECK_EQ(stats.p95_ms, 50.0);  // ceil(5*0.95)-1 = 4 → 50
  CHECK_EQ(stats.p99_ms, 50.0);
  CHECK_EQ(stats.max_ms, 50.0);
}

static void test_pending_eviction_on_overflow() {
  FakeClock clock;
  MetricsRegistry reg([&clock]() { return clock.now.load(); });
  const int idx = reg.register_stream("cam1");
  // 256 in-flight frames without ends; the 257th evicts the oldest.
  for (uint64_t f = 1; f <= 257; ++f) {
    reg.on_latency_begin(idx, f);
  }
  CHECK_EQ(reg.latency_evicted_count(idx), 1U);
  CHECK_EQ(reg.latency_stats(idx).samples, 0U);
}

static void test_pending_age_out() {
  FakeClock clock;
  MetricsRegistry reg([&clock]() { return clock.now.load(); });
  const int idx = reg.register_stream("cam1");
  reg.on_latency_begin(idx, 1);
  clock.advance_ms(6000);  // > 5s age limit
  reg.on_latency_begin(idx, 2);
  CHECK_EQ(reg.latency_evicted_count(idx), 1U);
  // The stale begin never pairs; ending frame 1 is now unknown.
  reg.on_latency_end(idx, 1);
  CHECK_EQ(reg.latency_desync_count(idx), 1U);
  CHECK_EQ(reg.latency_stats(idx).samples, 0U);
}

static void test_end_without_begin_desync() {
  FakeClock clock;
  MetricsRegistry reg([&clock]() { return clock.now.load(); });
  const int idx = reg.register_stream("cam1");
  reg.on_latency_end(idx, 42);
  CHECK_EQ(reg.latency_desync_count(idx), 1U);
  CHECK_EQ(reg.latency_stats(idx).samples, 0U);
}

static void test_since_window_slicing() {
  FakeClock clock;
  MetricsRegistry reg([&clock]() { return clock.now.load(); });
  const int idx = reg.register_stream("cam1");
  add_five_samples(&reg, idx, &clock);  // watermark 1..5
  const uint64_t before = reg.latency_watermark();
  CHECK_EQ(before, 5U);
  add_five_samples(&reg, idx, &clock, 100);  // watermark 6..10
  const auto windowed = reg.latency_stats_since(idx, before);
  CHECK_EQ(windowed.samples, 5U);
  CHECK_EQ(windowed.avg_ms, 30.0);
  const auto all = reg.latency_stats(idx);
  CHECK_EQ(all.samples, 10U);
}

static void test_watermark_monotonic_across_streams() {
  FakeClock clock;
  MetricsRegistry reg([&clock]() { return clock.now.load(); });
  const int a = reg.register_stream("cam1");
  const int b = reg.register_stream("cam2");
  add_five_samples(&reg, a, &clock);
  add_five_samples(&reg, b, &clock);
  CHECK_EQ(reg.latency_watermark(), 10U);
}

static void test_ring_capacity_bounds() {
  FakeClock clock;
  MetricsRegistry reg([&clock]() { return clock.now.load(); });
  const int idx = reg.register_stream("cam1");
  // 4097 samples: the oldest (10 ms) is evicted, newest 4096 survive.
  for (uint64_t f = 1; f <= 4097; ++f) {
    reg.on_latency_begin(idx, f);
    clock.advance_us(1000);
    reg.on_latency_end(idx, f);
  }
  const auto stats = reg.latency_stats(idx);
  CHECK_EQ(stats.samples, 4096U);
  // All surviving latencies are 1 ms.
  CHECK_EQ(stats.avg_ms, 1.0);
  CHECK_EQ(stats.max_ms, 1.0);
}

static void test_out_of_order_end_pairs_correct_frame() {
  FakeClock clock;
  MetricsRegistry reg([&clock]() { return clock.now.load(); });
  const int idx = reg.register_stream("cam1");
  reg.on_latency_begin(idx, 1);
  reg.on_latency_begin(idx, 2);
  // Ends arrive in FIFO order; scan finds frame 2 if frame 1 never ends.
  clock.advance_ms(5);
  reg.on_latency_end(idx, 2);
  CHECK_EQ(reg.latency_desync_count(idx), 0U);  // matched, no desync
  const auto stats = reg.latency_stats(idx);
  CHECK_EQ(stats.samples, 1U);
  CHECK_EQ(stats.max_ms, 5.0);
}

static void test_snapshot_carries_latency() {
  FakeClock clock;
  MetricsRegistry reg([&clock]() { return clock.now.load(); });
  const int idx = reg.register_stream("cam1");
  add_five_samples(&reg, idx, &clock);
  const auto summaries = reg.snapshot();
  CHECK_EQ(summaries.size(), 1U);
  CHECK_EQ(summaries[0].latency.samples, 5U);
  CHECK_EQ(summaries[0].latency.p95_ms, 50.0);
}

static void test_two_thread_stress() {
  FakeClock clock;
  MetricsRegistry reg([&clock]() { return clock.now.load(); });
  const int idx = reg.register_stream("cam1");
  std::atomic<bool> done{false};
  auto worker = [&]() {
    // Writer thread must never block on the fake clock (no clock reads in
    // begin/end beyond the injection); advance it from the writer.
    uint64_t f = 0;
    while (!done.load()) {
      reg.on_latency_begin(idx, ++f);
      clock.advance_us(1000);
      reg.on_latency_end(idx, f);
    }
  };
  std::thread t(worker);
  std::vector<MetricsRegistry::LatencyStats> stats;
  for (int i = 0; i < 50; ++i) {
    stats.push_back(reg.latency_stats(idx));
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  done.store(true);
  t.join();
  CHECK(stats.back().samples > 0);
  CHECK_EQ(stats.back().avg_ms, 1.0);
}

int main() {
  test_exact_percentiles();
  test_pending_eviction_on_overflow();
  test_pending_age_out();
  test_end_without_begin_desync();
  test_since_window_slicing();
  test_watermark_monotonic_across_streams();
  test_ring_capacity_bounds();
  test_out_of_order_end_pairs_correct_frame();
  test_snapshot_carries_latency();
  test_two_thread_stress();

  std::printf("test_metrics_registry: %d checks, %d failures\n",
              g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
