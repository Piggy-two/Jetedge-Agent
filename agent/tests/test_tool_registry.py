"""Tool registry + planner unit tests (Stage 12) — fake transport, no network."""

from __future__ import annotations

import unittest

from agent.goal_parser import parse_goal
from agent.planner import Planner
from agent.schemas import (
    TOOL_GET_ALL_STREAM_STATUS,
    TOOL_SET_INFER_INTERVAL,
    TOOL_SET_STREAM_PRIORITY,
    ToolCall,
)
from agent.tool_registry import ToolError, ToolRegistry, tool_schemas

GOAL = parse_goal("保证 cam1 推理 FPS 不低于 15,降低全局 P95 延迟")


class FakeHttp:
    """Records requests; returns canned envelopes."""

    def __init__(self):
        self.requests = []

    def _envelope(self, data=None):
        return data if data is not None else {}

    def get(self, path, timeout_s=None):
        self.requests.append(("GET", path, None, timeout_s))
        if path == "/health":
            return {"status": "ok"}
        if path == "/streams":
            return {"streams": []}
        if path == "/scheduler/state":
            return {"state": "NORMAL", "table_high": 0, "table_normal": 0,
                    "table_low": 0}
        if path == "/metrics/summary":
            return {"streams": []}
        return self._envelope()

    def post(self, path, body, timeout_s=None):
        self.requests.append(("POST", path, body, timeout_s))
        if path == "/config/rollback":
            return {"snapshot_id": body["snapshot_id"]}
        if path == "/config/snapshot":
            return {"snapshot_id": "snap_test_1"}
        if path == "/benchmark":
            return {"duration_s": body["duration_s"], "global": {},
                    "streams": {}}
        return {"ok": True}


class ToolRegistryTest(unittest.TestCase):
    def setUp(self):
        self.http = FakeHttp()
        self.tools = ToolRegistry(self.http)

    def test_interval_validation(self):
        with self.assertRaises(ToolError):
            self.tools.set_infer_interval("cam4", 9)
        with self.assertRaises(ToolError):
            self.tools.set_infer_interval("cam4", "1")
        self.tools.set_infer_interval("cam4", 2)
        self.assertEqual(self.http.requests[-1],
                         ("POST", "/streams/cam4/infer-interval",
                          {"interval": 2}, None))

    def test_priority_validation(self):
        with self.assertRaises(ToolError):
            self.tools.set_stream_priority("cam4", "ultra")
        self.tools.set_stream_priority("cam4", "low")

    def test_benchmark_timeout_is_duration_plus_slack(self):
        self.tools.run_benchmark(60)
        self.assertEqual(self.http.requests[-1][3], 80.0)

    def test_rollback_validation(self):
        with self.assertRaises(ToolError):
            self.tools.rollback_config("")
        self.tools.rollback_config("snap_1")
        self.assertEqual(self.http.requests[-1],
                         ("POST", "/config/rollback", {"snapshot_id": "snap_1"},
                          None))

    def test_unknown_tool(self):
        with self.assertRaises(ToolError):
            self.tools.call(ToolCall("rm_rf", {}))

    def test_tool_schemas_contain_safety_semantics(self):
        schemas = tool_schemas({"cam_id": "cam1"})
        names = [s["function"]["name"] for s in schemas]
        self.assertEqual(len(names), 7)
        interval_schema = next(s for s in schemas
                               if s["function"]["name"] == TOOL_SET_INFER_INTERVAL)
        desc = interval_schema["function"]["description"]
        self.assertIn("cam1", desc)          # goal camera hint
        self.assertIn("CRITICAL", desc)      # safety semantics
        self.assertIn("state switch", desc)  # override lifetime semantics


class PlannerTest(unittest.TestCase):
    def setUp(self):
        self.planner = Planner({"max_actions_per_round": 2, "max_infer_interval": 5})

    def _observation(self, streams):
        # Same shape the executor builds: keyed by tool name.
        return {TOOL_GET_ALL_STREAM_STATUS: {"streams": streams}}

    def test_deterministic_picks_low_priority_interval0(self):
        obs = self._observation([
            {"stream_id": "cam1", "priority": "high", "infer_interval": 0},
            {"stream_id": "cam2", "priority": "normal", "infer_interval": 0},
            {"stream_id": "cam3", "priority": "low", "infer_interval": 0},
            {"stream_id": "cam4", "priority": "low", "infer_interval": 2},
        ])
        plan = self.planner.plan(obs, GOAL, None)
        self.assertEqual(plan.source, "deterministic")
        # cam3 (lowest rank, interval 0) first, then cam2; never cam1.
        self.assertEqual([a.args["stream_id"] for a in plan.actions], ["cam3", "cam2"])
        self.assertTrue(all(a.args["interval"] == 1 for a in plan.actions))

    def test_llm_candidates_cleaned(self):
        obs = self._observation([])
        calls = [
            ToolCall(TOOL_SET_INFER_INTERVAL, {"stream_id": "cam4", "interval": 3}),
            ToolCall(TOOL_SET_INFER_INTERVAL, {"stream_id": "cam2", "interval": 9}),  # clamp
            ToolCall(TOOL_SET_INFER_INTERVAL, {"stream_id": "cam1", "interval": 2}),  # goal cam: drop
            ToolCall(TOOL_SET_STREAM_PRIORITY, {"stream_id": "cam3", "priority": "bogus"}),  # drop
        ]
        plan = self.planner.plan(obs, GOAL, calls)
        self.assertEqual(plan.source, "llm")
        self.assertEqual(len(plan.actions), 2)
        self.assertEqual(plan.actions[0].args,
                         {"stream_id": "cam4", "interval": 3})
        self.assertEqual(plan.actions[1].args,
                         {"stream_id": "cam2", "interval": 5})  # clamped

    def test_empty_llm_falls_back_to_deterministic(self):
        obs = self._observation([
            {"stream_id": "cam1", "priority": "high", "infer_interval": 0},
            {"stream_id": "cam4", "priority": "low", "infer_interval": 0},
        ])
        plan = self.planner.plan(obs, GOAL, [])
        self.assertEqual(plan.source, "deterministic")
        self.assertEqual(plan.actions[0].args["stream_id"], "cam4")


if __name__ == "__main__":
    unittest.main()
