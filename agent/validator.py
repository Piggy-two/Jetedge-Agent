"""Goal verification — pure functions, no HTTP (Stage 12).

The verdict (keep / rollback) is always computed by this deterministic code,
never by the LLM.  Decision rules:

  1. Data integrity: every measured stream complete; global P95 present.
  2. Scheduler noise: a state switch inside the measurement window makes the
     window suspect (manual interval overrides are re-asserted by the policy
     table on switch) — caller re-measures once; this function just flags it.
  3. Goal: global P95 must drop by BOTH the relative threshold AND the
     absolute threshold (dual guard vs measurement noise).  If the baseline
     P95 is already below the trivial bound, the goal counts as met without
     any change.
  4. cam1 floor: input_fps >= min_fps * (1 - tolerance).
  5. drop rate must not worsen beyond tolerance.
"""

from __future__ import annotations

from agent.schemas import BenchmarkResult, VerificationResult


class ValidationError(Exception):
    """Raised when benchmark data is unusable (no verdict can be formed)."""


def check_goal_met(before: BenchmarkResult, after: BenchmarkResult, goal) -> VerificationResult:
    reasons = []
    if before.scheduler_state_after == "CRITICAL" or after.scheduler_state_after == "CRITICAL":
        raise ValidationError("verification window entered CRITICAL — measurement "
                              "untrustworthy, aborting")

    if after.scheduler_state_before != after.scheduler_state_after:
        reasons.append("scheduler state switched inside the after-window "
                       "(manual intervals may have been re-asserted)")

    if before.global_p95_ms is None or after.global_p95_ms is None:
        raise ValidationError("global P95 unavailable in before/after benchmark")
    for sid, s in after.streams.items():
        if not s.complete:
            reasons.append(f"stream {sid}: after-window incomplete "
                           f"({s.frames_in} frames)")

    if before.global_p95_ms < goal.p95_trivial_ms:
        return VerificationResult(
            ok=True,
            already_met=True,
            reasons=[f"global P95 already {before.global_p95_ms:.1f} ms "
                     f"(< {goal.p95_trivial_ms:.0f} ms): no change needed"],
        )

    improved = (
        after.global_p95_ms <= before.global_p95_ms * (1.0 - goal.p95_reduction_ratio)
        and after.global_p95_ms <= before.global_p95_ms - goal.p95_reduction_abs_ms
    )
    if not improved:
        reasons.append(
            f"global P95 not reduced enough: "
            f"{before.global_p95_ms:.1f} → {after.global_p95_ms:.1f} ms "
            f"(need ≤ {before.global_p95_ms * (1.0 - goal.p95_reduction_ratio):.1f} ms "
            f"and ≤ {before.global_p95_ms - goal.p95_reduction_abs_ms:.1f} ms)")

    cam = after.stream(goal.cam_id)
    if cam is None:
        reasons.append(f"goal camera {goal.cam_id} missing from after-window")
    elif cam.input_fps < goal.min_fps * (1.0 - goal.fps_tolerance_ratio):
        reasons.append(
            f"{goal.cam_id} inference FPS {cam.input_fps:.1f} below floor "
            f"{goal.min_fps:.1f} (tolerance {goal.fps_tolerance_ratio:.0%})")

    before_dr = before.global_drop_rate
    after_dr = after.global_drop_rate
    if after_dr > max(before_dr + goal.drop_rate_tolerance_pp, goal.drop_rate_floor):
        reasons.append(
            f"drop rate worsened: {before_dr * 100.0:.2f}% → "
            f"{after_dr * 100.0:.2f}%")

    return VerificationResult(ok=not reasons, reasons=reasons)
