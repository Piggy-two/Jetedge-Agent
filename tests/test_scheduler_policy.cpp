// Unit tests for SchedulerPolicy (Stage 9 deterministic scheduler).
// Pure logic — no GStreamer dependency.

#include <cstdio>

#include "jetedge/scheduler/scheduler_policy.h"

using jetedge::scheduler::PolicyTable;
using jetedge::scheduler::SchedulerConfig;
using jetedge::scheduler::SchedulerPolicy;
using jetedge::scheduler::SchedulerState;
using jetedge::scheduler::SystemSample;

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

static SchedulerConfig mk() {
  SchedulerConfig c;
  c.enable = true;
  c.sample_interval_sec = 1;
  c.pressure_cpu_enter = 70.0;
  c.pressure_cpu_exit = 50.0;
  c.thermal_temp_enter = 75.0;
  c.thermal_temp_exit = 70.0;
  c.critical_temp_enter = 82.0;
  c.critical_temp_exit = 76.0;
  c.min_hold_ms = 1000;
  c.cooldown_ms = 2000;
  c.max_adjustments_per_window = 2;
  c.adjust_window_ms = 60000;
  return c;
}

static void test_initial_state() {
  SchedulerPolicy p(mk());
  CHECK(p.state() == SchedulerState::kNormal);
  CHECK(p.table() == (PolicyTable{0, 0, 0}));
  CHECK(p.recovery_step() == 0);
  CHECK(p.adjustments_in_window() == 0);
}

static void test_hysteresis_cpu_below_enter() {
  SchedulerPolicy p(mk());
  SystemSample s;
  s.cpu_pct = 60.0;  // < enter(70)
  CHECK(!p.update(s, 1000));
  CHECK(p.state() == SchedulerState::kNormal);
}

static void test_pressure_entry() {
  SchedulerPolicy p(mk());
  SystemSample s;
  s.cpu_pct = 85.0;  // >= enter(70)
  CHECK(p.update(s, 1000));  // hold(1000ms) elapsed since construction
  CHECK(p.state() == SchedulerState::kPressure);
  CHECK(p.table() == (PolicyTable{0, 1, 2}));
}

static void test_min_hold_blocks_escalation() {
  SchedulerPolicy p(mk());
  SystemSample s;
  s.cpu_pct = 85.0;
  CHECK(!p.update(s, 500));  // 500ms < min_hold(1000ms)
  CHECK(p.state() == SchedulerState::kNormal);
}

static void test_full_pressure_recovery_cycle() {
  SchedulerPolicy p(mk());
  SystemSample s;
  s.cpu_pct = 85.0;
  p.update(s, 1000);
  CHECK(p.state() == SchedulerState::kPressure);

  s.cpu_pct = 30.0;  // <= exit(50)
  p.update(s, 2000);  // hold elapsed → RECOVERY stage 0
  CHECK(p.state() == SchedulerState::kRecovery);
  CHECK(p.recovery_step() == 0);
  CHECK(p.table() == (PolicyTable{0, 1, 2}));  // same as PRESSURE — no jump

  p.update(s, 3000);  // stage 1
  CHECK(p.recovery_step() == 1);
  CHECK(p.table() == (PolicyTable{0, 0, 1}));

  p.update(s, 4000);  // stage 2
  CHECK(p.recovery_step() == 2);
  CHECK(p.table() == (PolicyTable{0, 0, 0}));

  p.update(s, 5000);  // NORMAL
  CHECK(p.state() == SchedulerState::kNormal);
  CHECK(p.table() == (PolicyTable{0, 0, 0}));
}

static void test_cooldown_blocks_re_escalation() {
  SchedulerPolicy p(mk());
  SystemSample s;
  s.cpu_pct = 85.0;
  p.update(s, 1000);   // PRESSURE
  s.cpu_pct = 30.0;
  p.update(s, 2000);   // RECOVERY
  p.update(s, 3000);   // stage 1
  p.update(s, 4000);   // stage 2
  p.update(s, 5000);   // NORMAL (cooldown until 7000)
  CHECK(p.state() == SchedulerState::kNormal);

  s.cpu_pct = 85.0;
  CHECK(!p.update(s, 6000));  // cooldown active → no re-escalation
  CHECK(p.state() == SchedulerState::kNormal);

  p.update(s, 7000);   // cooldown expired (7000 >= 7000) → PRESSURE
  CHECK(p.state() == SchedulerState::kPressure);
}

static void test_thermal_precedence() {
  // From NORMAL: temperature dominates low CPU.
  SchedulerPolicy p(mk());
  SystemSample s;
  s.cpu_pct = 20.0;
  s.temp_c = 80.0;  // >= thermal enter(75)
  p.update(s, 1000);
  CHECK(p.state() == SchedulerState::kThermal);
  CHECK(p.table() == (PolicyTable{0, 2, 3}));

  // From PRESSURE: temperature escalates PRESSURE → THERMAL.
  SchedulerPolicy p2(mk());
  SystemSample s2;
  s2.cpu_pct = 85.0;
  p2.update(s2, 1000);
  CHECK(p2.state() == SchedulerState::kPressure);
  s2.temp_c = 80.0;
  p2.update(s2, 2000);
  CHECK(p2.state() == SchedulerState::kThermal);
}

static void test_critical_immediate_and_invariant() {
  // CRITICAL is safety-dominant: entered immediately (no min_hold/cooldown).
  SchedulerPolicy p(mk());
  SystemSample s;
  s.temp_c = 85.0;  // >= critical enter(82)
  CHECK(p.update(s, 1000));
  CHECK(p.state() == SchedulerState::kCritical);
  CHECK(p.table() == (PolicyTable{1, 3, 15}));  // low ≈ paused (bounded)

  // No-load-increase invariant: CRITICAL table >= table before escalation
  // (checked against PRESSURE and THERMAL tables).
  const PolicyTable critical = p.table();
  CHECK(critical.high >= 0 && critical.normal >= 0 && critical.low >= 0);
  CHECK(critical.high >= (PolicyTable{0, 1, 2}).high);
  CHECK(critical.normal >= (PolicyTable{0, 1, 2}).normal);
  CHECK(critical.low >= (PolicyTable{0, 1, 2}).low);
  CHECK(critical.low >= (PolicyTable{0, 2, 3}).low);

  // Exit to RECOVERY once cooled below critical exit(76), after hold.
  s.temp_c = 70.0;
  p.update(s, 2000);
  CHECK(p.state() == SchedulerState::kRecovery);
}

static void test_budget_blocks_escalation_only() {
  SchedulerConfig c = mk();
  c.max_adjustments_per_window = 1;
  SchedulerPolicy p(c);
  SystemSample s;
  s.cpu_pct = 85.0;
  p.update(s, 1000);  // PRESSURE — budget 1/1 used
  CHECK(p.state() == SchedulerState::kPressure);
  CHECK(p.adjustments_in_window() == 1);

  s.temp_c = 80.0;  // THERMAL requested but budget exhausted → suppressed
  p.update(s, 2000);
  CHECK(p.state() == SchedulerState::kPressure);

  s.temp_c = -1.0;
  s.cpu_pct = 30.0;
  p.update(s, 3000);  // de-escalation never blocked by budget → RECOVERY
  CHECK(p.state() == SchedulerState::kRecovery);
}

static void test_re_escalation_from_recovery() {
  SchedulerPolicy p(mk());
  SystemSample s;
  s.cpu_pct = 85.0;
  p.update(s, 1000);  // PRESSURE
  s.cpu_pct = 30.0;
  p.update(s, 2000);  // RECOVERY stage 0
  CHECK(p.state() == SchedulerState::kRecovery);

  s.temp_c = 80.0;  // pressure returns while recovering → THERMAL
  p.update(s, 3000);
  CHECK(p.state() == SchedulerState::kThermal);
}

static void test_missing_metrics_never_trap() {
  // All readings missing → policy stays NORMAL (no false throttling).
  SchedulerPolicy p(mk());
  SystemSample s;  // cpu/mem/temp all -1
  CHECK(!p.update(s, 1000));
  CHECK(p.state() == SchedulerState::kNormal);

  // Thermal reading disappears while CRITICAL → recovery is not blocked.
  SchedulerPolicy p2(mk());
  SystemSample s2;
  s2.temp_c = 85.0;
  p2.update(s2, 1000);
  CHECK(p2.state() == SchedulerState::kCritical);
  s2.temp_c = -1.0;
  p2.update(s2, 2000);
  CHECK(p2.state() == SchedulerState::kRecovery);

  // Missing CPU reading never blocks recovery stepping.
  SchedulerPolicy p3(mk());
  SystemSample s3;
  s3.cpu_pct = 85.0;
  p3.update(s3, 1000);  // PRESSURE
  s3.cpu_pct = -1.0;
  p3.update(s3, 2000);  // CPU reading gone → still recovers (missing = not pressing)
  CHECK(p3.state() == SchedulerState::kRecovery);
}

static void test_adjust_window_reset() {
  SchedulerConfig c = mk();
  c.max_adjustments_per_window = 1;
  c.adjust_window_ms = 5000;
  SchedulerPolicy p(c);
  SystemSample s;
  s.cpu_pct = 85.0;
  p.update(s, 1000);  // PRESSURE — 1/1 used
  CHECK(p.adjustments_in_window() == 1);

  s.cpu_pct = 30.0;
  p.update(s, 2000);  // RECOVERY (de-escalation, no budget consumed)
  CHECK(p.state() == SchedulerState::kRecovery);

  s.cpu_pct = 85.0;
  p.update(s, 3000);  // re-escalation still blocked (budget, hold elapsed)
  CHECK(p.state() == SchedulerState::kRecovery);

  p.update(s, 7000);  // window rolled (7000 >= 1000+5000) → budget reset,
                      // escalation allowed again → PRESSURE
  CHECK(p.adjustments_in_window() == 1);
  CHECK(p.state() == SchedulerState::kPressure);
}

int main() {
  test_initial_state();
  test_hysteresis_cpu_below_enter();
  test_pressure_entry();
  test_min_hold_blocks_escalation();
  test_full_pressure_recovery_cycle();
  test_cooldown_blocks_re_escalation();
  test_thermal_precedence();
  test_critical_immediate_and_invariant();
  test_budget_blocks_escalation_only();
  test_re_escalation_from_recovery();
  test_missing_metrics_never_trap();
  test_adjust_window_reset();

  std::printf("scheduler_policy: %d checks, %d failures\n", g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}
