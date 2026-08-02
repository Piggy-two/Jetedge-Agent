// SchedulerPolicy — deterministic runtime scheduler state machine (Stage 9).
//
// Pure logic, no GStreamer dependency (unit-testable in isolation).
// State flow (CLAUDE.md §15):
//
//   NORMAL → PRESSURE → RECOVERY → NORMAL
//      │        │   ↘              ↑
//      │        ↘→ THERMAL → RECOVERY
//      │             │
//      ↘→ CRITICAL ←─┘   (temp ≥ critical_temp_enter, from any state)
//
// Properties enforced by the policy:
//   - hysteresis: enter/exit thresholds differ (no thrashing at the boundary)
//   - min_hold:   a state must be held for min_hold_ms before it may change
//   - cooldown:   after returning to NORMAL, re-escalation is blocked for
//                 cooldown_ms (anti-oscillation)
//   - budget:     at most max_adjustments_per_window escalations per
//                 adjust_window_ms; de-escalations are never blocked
//   - thermal precedence: temperature signals dominate CPU-based ones
//   - CRITICAL never increases load: its interval table is the maximum, and
//     the only allowed transition out is RECOVERY (load only decreases)
//   - missing metrics never trap the system: CRITICAL exits when the thermal
//     reading disappears, RECOVERY steps down without pressure data
//
// Output: per-priority-tier inference interval (0 = infer every frame;
// interval k = keep every (k+1)-th frame).  RECOVERY steps the table down
// monotonically stage by stage back to NORMAL ("逐级恢复配置").

#pragma once

#include <cstdint>

namespace jetedge {
namespace scheduler {

enum class SchedulerState {
  kNormal,    // full rate
  kPressure,  // CPU above threshold — increase inference interval
  kThermal,   // temperature above threshold — throttle low-priority streams
  kCritical,  // temperature critical — pause low-priority inference, no load increase
  kRecovery,  // load recovered — restore configuration gradually
};

const char* scheduler_state_str(SchedulerState s);

// Inference interval per priority tier (0 = every frame).
struct PolicyTable {
  int high = 0;
  int normal = 0;
  int low = 0;
  bool operator==(const PolicyTable& o) const {
    return high == o.high && normal == o.normal && low == o.low;
  }
};

// One tick of sampled system state.  -1 = reading unavailable.
struct SystemSample {
  double cpu_pct = -1.0;  // CPU busy fraction over the sampling window
  double mem_pct = -1.0;  // used RAM percentage
  double temp_c = -1.0;   // max readable thermal-zone temperature (°C)
};

struct SchedulerConfig {
  bool enable = false;           // master switch (default off → inert)
  int sample_interval_sec = 2;   // driver tick interval
  // Hysteresis thresholds (enter > exit).
  double pressure_cpu_enter = 70.0;   // % CPU to enter PRESSURE
  double pressure_cpu_exit = 50.0;    // % CPU to leave PRESSURE
  double thermal_temp_enter = 75.0;   // °C to enter THERMAL
  double thermal_temp_exit = 70.0;    // °C to leave THERMAL
  double critical_temp_enter = 82.0;  // °C to enter CRITICAL
  double critical_temp_exit = 76.0;   // °C to leave CRITICAL
  uint64_t min_hold_ms = 15000;       // minimum time in a state
  uint64_t cooldown_ms = 30000;       // re-escalation block after NORMAL return
  int max_adjustments_per_window = 2; // escalation budget per window
  uint64_t adjust_window_ms = 120000;
};

class SchedulerPolicy {
 public:
  explicit SchedulerPolicy(SchedulerConfig cfg);

  SchedulerState state() const { return state_; }
  const PolicyTable& table() const { return table_; }
  int recovery_step() const { return recovery_step_; }
  int adjustments_in_window() const { return adjustments_; }

  // Feed one system sample at monotonic time `now_ms`.  Returns true when the
  // output table changed (caller applies intervals / logs the transition).
  bool update(const SystemSample& s, uint64_t now_ms);

 private:
  PolicyTable table_for_state(SchedulerState st) const;
  void enter(SchedulerState st, uint64_t now_ms);
  bool budget_ok() const { return adjustments_ < cfg_.max_adjustments_per_window; }

  SchedulerConfig cfg_;
  SchedulerState state_ = SchedulerState::kNormal;
  PolicyTable table_ = {0, 0, 0};
  uint64_t state_since_ms_ = 0;
  uint64_t cooldown_until_ms_ = 0;
  uint64_t window_start_ms_ = 0;
  int adjustments_ = 0;
  int recovery_step_ = 0;
};

}  // namespace scheduler
}  // namespace jetedge
