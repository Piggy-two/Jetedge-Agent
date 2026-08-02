// SchedulerPolicy implementation (Stage 9).

#include "jetedge/scheduler/scheduler_policy.h"

#include <algorithm>

namespace jetedge {
namespace scheduler {

namespace {

// CRITICAL low-priority interval.  A true full pause (drop every frame) is
// intentionally NOT used: a live source that never delivers any frame can
// starve nvstreammux's live-mode first batch.  interval=15 keeps the stream
// at ~1/16 of its source rate (~2 fps at 30 fps), effectively paused.
constexpr int kPausedInterval = 15;

// RECOVERY stages: monotone per-tier decrease back to NORMAL.  The table is
// entered at stage 0 regardless of the previous state (all intervals either
// stay or shrink — "逐级恢复配置").
const PolicyTable kRecoveryStages[] = {
    {0, 1, 2},
    {0, 0, 1},
    {0, 0, 0},
};
constexpr int kNumRecoveryStages = 3;

}  // namespace

const char* scheduler_state_str(SchedulerState s) {
  switch (s) {
    case SchedulerState::kNormal:   return "NORMAL";
    case SchedulerState::kPressure: return "PRESSURE";
    case SchedulerState::kThermal:  return "THERMAL";
    case SchedulerState::kCritical: return "CRITICAL";
    case SchedulerState::kRecovery: return "RECOVERY";
  }
  return "???";
}

SchedulerPolicy::SchedulerPolicy(SchedulerConfig cfg) : cfg_(cfg) {}

PolicyTable SchedulerPolicy::table_for_state(SchedulerState st) const {
  switch (st) {
    case SchedulerState::kNormal:   return {0, 0, 0};
    case SchedulerState::kPressure: return {0, 1, 2};
    case SchedulerState::kThermal:  return {0, 2, 3};
    case SchedulerState::kCritical: return {1, 3, kPausedInterval};
    case SchedulerState::kRecovery:
      return kRecoveryStages[std::min(recovery_step_, kNumRecoveryStages - 1)];
  }
  return {0, 0, 0};
}

void SchedulerPolicy::enter(SchedulerState st, uint64_t now_ms) {
  const bool escalation = (st == SchedulerState::kPressure ||
                           st == SchedulerState::kThermal ||
                           st == SchedulerState::kCritical);
  if (escalation) {
    ++adjustments_;
  }
  state_ = st;
  state_since_ms_ = now_ms;
  recovery_step_ = 0;
  if (st == SchedulerState::kNormal) {
    // Anti-oscillation: after a full recovery, block quick re-escalation.
    cooldown_until_ms_ = now_ms + cfg_.cooldown_ms;
  }
}

bool SchedulerPolicy::update(const SystemSample& s, uint64_t now_ms) {
  const SchedulerState prev_state = state_;
  const PolicyTable prev_table = table_;

  // Pressure signals (missing readings are treated as "not pressing"; they
  // never block de-escalation, and never trap the system in a high state).
  const bool hot_critical = s.temp_c >= 0 && s.temp_c >= cfg_.critical_temp_enter;
  const bool hot = s.temp_c >= 0 && s.temp_c >= cfg_.thermal_temp_enter;
  const bool cooled = (s.temp_c < 0) || s.temp_c <= cfg_.thermal_temp_exit;
  const bool cooled_critical = (s.temp_c < 0) || s.temp_c <= cfg_.critical_temp_exit;
  const bool cpu_high = s.cpu_pct >= 0 && s.cpu_pct >= cfg_.pressure_cpu_enter;
  const bool cpu_low = (s.cpu_pct < 0) || s.cpu_pct <= cfg_.pressure_cpu_exit;

  const bool hold_elapsed = (now_ms - state_since_ms_) >= cfg_.min_hold_ms;
  const bool cooldown_ok = now_ms >= cooldown_until_ms_;

  // Escalation budget window rolls forward.
  if (now_ms >= window_start_ms_ + cfg_.adjust_window_ms) {
    window_start_ms_ = now_ms;
    adjustments_ = 0;
  }

  switch (state_) {
    case SchedulerState::kNormal:
      // CRITICAL is safety-dominant: never blocked by cooldown or budget.
      if (hot_critical) {
        enter(SchedulerState::kCritical, now_ms);
        break;
      }
      if (!cooldown_ok) {
        break;  // just recovered — no immediate re-escalation
      }
      if (hot && hold_elapsed && budget_ok()) {
        enter(SchedulerState::kThermal, now_ms);
        break;
      }
      if (cpu_high && hold_elapsed && budget_ok()) {
        enter(SchedulerState::kPressure, now_ms);
        break;
      }
      break;

    case SchedulerState::kPressure:
      if (hot_critical) {
        enter(SchedulerState::kCritical, now_ms);
        break;
      }
      if (hot && hold_elapsed && budget_ok()) {
        enter(SchedulerState::kThermal, now_ms);  // thermal precedence
        break;
      }
      if (cpu_low && hold_elapsed) {
        enter(SchedulerState::kRecovery, now_ms);
      }
      break;

    case SchedulerState::kThermal:
      if (hot_critical) {
        enter(SchedulerState::kCritical, now_ms);
        break;
      }
      if (cooled && hold_elapsed) {
        enter(SchedulerState::kRecovery, now_ms);
      }
      break;

    case SchedulerState::kCritical:
      // In CRITICAL nothing may increase load: the table is the maximum and
      // the only exit is RECOVERY (intervals only shrink from here).
      if (cooled_critical && hold_elapsed) {
        enter(SchedulerState::kRecovery, now_ms);
      }
      break;

    case SchedulerState::kRecovery:
      if (hot_critical) {
        enter(SchedulerState::kCritical, now_ms);
        break;
      }
      if (hot && hold_elapsed && budget_ok()) {
        enter(SchedulerState::kThermal, now_ms);
        break;
      }
      if (cpu_high && hold_elapsed && budget_ok()) {
        enter(SchedulerState::kPressure, now_ms);
        break;
      }
      // Step down one stage while the system stays healthy.
      if (hold_elapsed && cooled && cpu_low) {
        if (recovery_step_ < kNumRecoveryStages - 1) {
          ++recovery_step_;
          state_since_ms_ = now_ms;
        } else {
          enter(SchedulerState::kNormal, now_ms);
        }
      }
      break;
  }

  table_ = table_for_state(state_);
  return state_ != prev_state || !(table_ == prev_table);
}

}  // namespace scheduler
}  // namespace jetedge
