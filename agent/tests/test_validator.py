"""Validator unit tests (Stage 12) — pure decision rules, no HTTP."""

from __future__ import annotations

import unittest

from agent.goal_parser import parse_goal
from agent.schemas import BenchmarkResult, StreamBench
from agent.validator import ValidationError, check_goal_met

GOAL = parse_goal("保证 cam1 推理 FPS 不低于 15,降低全局 P95 延迟")


def make_bench(p95, cam_fps=29.0, drop=0.001, complete=True, state="NORMAL",
               global_fps=29.0):
    b = BenchmarkResult(
        duration_s=10, elapsed_ms=10000,
        scheduler_state_before=state, scheduler_state_after=state,
        global_p95_ms=p95, global_drop_rate=drop, global_input_fps=global_fps,
    )
    b.streams["cam1"] = StreamBench(input_fps=cam_fps, complete=complete,
                                    drop_rate=drop)
    b.streams["cam2"] = StreamBench(input_fps=29.0, complete=complete,
                                    drop_rate=drop)
    return b


class ValidatorTest(unittest.TestCase):
    def test_clear_improvement_keeps(self):
        before = make_bench(40.0)
        after = make_bench(30.0)  # 25% + 10ms → both thresholds met
        verdict = check_goal_met(before, after, GOAL)
        self.assertTrue(verdict.ok, verdict.reasons)
        self.assertFalse(verdict.already_met)

    def test_insufficient_improvement_rolls_back(self):
        before = make_bench(40.0)
        after = make_bench(36.0)  # 10% but only 4ms — absolute threshold unmet
        verdict = check_goal_met(before, after, GOAL)
        self.assertFalse(verdict.ok)
        self.assertTrue(any("P95" in r for r in verdict.reasons))

    def test_trivial_baseline_already_met(self):
        before = make_bench(12.0)
        after = make_bench(13.0)
        verdict = check_goal_met(before, after, GOAL)
        self.assertTrue(verdict.ok)
        self.assertTrue(verdict.already_met)

    def test_cam1_fps_floor_enforced(self):
        before = make_bench(40.0)
        after = make_bench(28.0, cam_fps=10.0)  # below 15 * 0.95
        verdict = check_goal_met(before, after, GOAL)
        self.assertFalse(verdict.ok)
        self.assertTrue(any("FPS" in r for r in verdict.reasons))

    def test_drop_rate_worsened_rolls_back(self):
        before = make_bench(40.0, drop=0.001)
        after = make_bench(28.0, drop=0.05)
        verdict = check_goal_met(before, after, GOAL)
        self.assertFalse(verdict.ok)
        self.assertTrue(any("drop" in r for r in verdict.reasons))

    def test_incomplete_stream_flags(self):
        before = make_bench(40.0)
        after = make_bench(28.0)
        after.streams["cam2"] = StreamBench(input_fps=29.0, complete=False,
                                            frames_in=2)
        verdict = check_goal_met(before, after, GOAL)
        self.assertFalse(verdict.ok)
        self.assertTrue(any("incomplete" in r for r in verdict.reasons))

    def test_scheduler_switch_flags_remeasure(self):
        before = make_bench(40.0)
        after = make_bench(28.0)
        after.scheduler_state_before = "NORMAL"
        after.scheduler_state_after = "PRESSURE"
        verdict = check_goal_met(before, after, GOAL)
        self.assertFalse(verdict.ok)  # suspect window: verdict must be no-keep
        self.assertTrue(any("switch" in r for r in verdict.reasons))

    def test_critical_window_raises(self):
        before = make_bench(40.0, state="NORMAL")
        after = make_bench(28.0, state="CRITICAL")
        with self.assertRaises(ValidationError):
            check_goal_met(before, after, GOAL)

    def test_missing_p95_raises(self):
        before = make_bench(40.0)
        after = make_bench(None)
        with self.assertRaises(ValidationError):
            check_goal_met(before, after, GOAL)


if __name__ == "__main__":
    unittest.main()
