"""Executor loop integration tests (Stage 12) — fake HTTP, no network.

Covers both outcomes: goal met → keep (exit ok); goal not met → automatic
rollback to the baseline snapshot with read-back verification.
"""

from __future__ import annotations

import unittest

from agent.audit import AgentAudit
from agent.executor import Executor
from agent.goal_parser import parse_goal
from agent.planner import Planner
from agent.tool_registry import ToolRegistry

GOAL = parse_goal("保证 cam1 推理 FPS 不低于 15,降低全局 P95 延迟")


def make_bench_data(p95, cam_fps=29.0, state="NORMAL", frames_in=300):
    return {
        "duration_s": 5, "elapsed_ms": 5000,
        "scheduler_state_before": state, "scheduler_state_after": state,
        "table_before": {"high": 0, "normal": 0, "low": 0},
        "table_after": {"high": 0, "normal": 0, "low": 0},
        "streams": {
            "cam1": {"frames_in": frames_in, "frames_out": frames_in,
                     "input_fps": cam_fps, "output_fps": cam_fps,
                     "drop_rate": 0.0, "complete": True,
                     "latency": {"samples": frames_in, "avg_ms": 18.0,
                                 "p50_ms": 15.0, "p95_ms": p95,
                                 "p99_ms": p95 * 1.2, "max_ms": p95 * 1.5}},
            "cam2": {"frames_in": frames_in, "frames_out": frames_in,
                     "input_fps": 29.0, "output_fps": 29.0,
                     "drop_rate": 0.0, "complete": True,
                     "latency": {"samples": frames_in, "avg_ms": 18.0,
                                 "p50_ms": 15.0, "p95_ms": p95,
                                 "p99_ms": p95 * 1.2, "max_ms": p95 * 1.5}},
        },
        "global": {"samples": frames_in * 2, "avg_ms": 18.0, "p50_ms": 15.0,
                   "p95_ms": p95, "p99_ms": p95 * 1.2, "max_ms": p95 * 1.5,
                   "input_fps": 29.0, "drop_rate": 0.0},
    }


class ScriptedHttp:
    """A fake transport that serves a small state machine:

      - benchmark returns p95 from the current scripted value (40.0 ms for
        the before window, 28.0 for after when `improves` is True);
      - write ops mutate the in-memory interval so read-back verification
        reflects them;
      - rollback restores the baseline values.
    """

    def __init__(self, improves: bool, cam_fps: float = 29.0):
        self.improves = improves
        self.cam_fps = cam_fps
        self.bench_call = 0
        self.interval = {"cam1": 0, "cam2": 0, "cam3": 0, "cam4": 0}
        self.baseline = dict(self.interval)
        self.rollbacks = 0
        self.snapshots = []
        self.snapshot_counter = 0

    def _streams(self):
        return {"streams": [
            {"stream_id": "cam1", "state": "RUNNING", "priority": "high",
             "infer_interval": self.interval["cam1"], "frames": 1000},
            {"stream_id": "cam2", "state": "RUNNING", "priority": "normal",
             "infer_interval": self.interval["cam2"], "frames": 1000},
            {"stream_id": "cam3", "state": "RUNNING", "priority": "normal",
             "infer_interval": self.interval["cam3"], "frames": 1000},
            {"stream_id": "cam4", "state": "RUNNING", "priority": "low",
             "infer_interval": self.interval["cam4"], "frames": 1000},
        ]}

    def get(self, path, timeout_s=None):
        if path == "/health":
            return {"status": "ok"}
        if path == "/streams":
            return self._streams()
        if path == "/scheduler/state":
            return {"state": "NORMAL", "enabled": True, "table_high": 0,
                    "table_normal": 0, "table_low": 0}
        if path == "/metrics/summary":
            return {"streams": []}
        return {}

    def post(self, path, body, timeout_s=None):
        if path == "/config/snapshot":
            self.snapshot_counter += 1
            sid = f"snap_{self.snapshot_counter}"
            self.baseline = dict(self.interval)
            self.snapshots.append(sid)
            return {"snapshot_id": sid}
        if path == "/config/rollback":
            self.interval = dict(self.baseline)
            self.rollbacks += 1
            return {"snapshot_id": body["snapshot_id"]}
        if path == "/benchmark":
            self.bench_call += 1
            p95 = 28.0 if (self.improves and self.bench_call > 1) else 40.0
            return make_bench_data(p95, cam_fps=self.cam_fps)
        if path.endswith("/infer-interval"):
            sid = path.split("/")[-2]
            self.interval[sid] = body["interval"]
            return {"stream_id": sid, "infer_interval": body["interval"]}
        return {}


def run_executor(http, cfg=None):
    import tempfile
    import os

    overrides = {"benchmark_duration_s": 5, "max_change_rounds": 2,
                 "deadline_s": 120, "observe_timeout_s": 10.0}
    if cfg:
        overrides.update(cfg)
    base = dict({
        "benchmark_duration_s": 5, "max_change_rounds": 2, "deadline_s": 120,
        "observe_timeout_s": 10.0, "max_actions_per_round": 2,
        "max_infer_interval": 5,
        "report_dir": "/tmp", "audit_file": "",
    })
    base.update(overrides)
    tools = ToolRegistry(http)
    planner = Planner(base)
    audit = AgentAudit("", "test-run")
    executor = Executor(base, tools, planner, audit, llm=None)
    return executor, audit


class ExecutorTest(unittest.TestCase):
    def test_goal_met_keeps_config(self):
        http = ScriptedHttp(improves=True)
        executor, audit = run_executor(http)
        result = executor.run(GOAL)
        audit.close()
        self.assertTrue(result.ok)
        # The deterministic plan raised the two lowest-rank interval-0 streams
        # (cam4 low, cam2 normal) to 1, and the change was kept.
        self.assertEqual(http.interval["cam4"], 1)
        self.assertEqual(http.interval["cam2"], 1)
        self.assertEqual(http.interval["cam3"], 0)  # untouched (rank order)
        self.assertEqual(http.interval["cam1"], 0)  # goal camera untouched
        self.assertEqual(http.rollbacks, 0)

    def test_goal_not_met_rolls_back(self):
        http = ScriptedHttp(improves=False)
        executor, audit = run_executor(http)
        result = executor.run(GOAL)
        audit.close()
        self.assertFalse(result.ok)
        # Everything restored to the baseline snapshot.
        self.assertEqual(http.interval, {"cam1": 0, "cam2": 0, "cam3": 0,
                                         "cam4": 0})
        self.assertGreaterEqual(http.rollbacks, 1)
        self.assertGreaterEqual(len(http.snapshots), 1)

    def test_cam1_floor_violated_rolls_back(self):
        http = ScriptedHttp(improves=True, cam_fps=5.0)  # below 15 * 0.95
        executor, audit = run_executor(http)
        result = executor.run(GOAL)
        audit.close()
        self.assertFalse(result.ok)
        self.assertEqual(http.interval, {"cam1": 0, "cam2": 0, "cam3": 0,
                                         "cam4": 0})


if __name__ == "__main__":
    unittest.main()
