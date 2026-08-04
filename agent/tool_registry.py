"""Whitelisted tool registry (Stage 12, implementation_plan §64).

Seven tools, each a thin wrapper over one Control API endpoint.  Every call
re-validates types/ranges defensively before hitting the HTTP layer, and
every result is returned as parsed JSON (real values only — the executor
never fabricates numbers).

Tools:
  get_system_metrics()      GET  /metrics/summary + /scheduler/state
  get_all_stream_status()   GET  /streams
  get_scheduler_state()     GET  /scheduler/state
  set_infer_interval(id,n)  POST /streams/<id>/infer-interval
  set_stream_priority(id,p) POST /streams/<id>/priority
  run_benchmark(duration)   POST /benchmark
  rollback_config(snap)     POST /config/rollback
"""

from __future__ import annotations

from typing import Optional

from agent.http_client import HttpTransport
from agent.schemas import (
    TOOL_GET_ALL_STREAM_STATUS,
    TOOL_GET_SCHEDULER_STATE,
    TOOL_GET_SYSTEM_METRICS,
    TOOL_ROLLBACK_CONFIG,
    TOOL_RUN_BENCHMARK,
    TOOL_SET_INFER_INTERVAL,
    TOOL_SET_STREAM_PRIORITY,
    ToolCall,
)

VALID_PRIORITIES = ("high", "normal", "low")


class ToolError(Exception):
    def __init__(self, code: str, message: str):
        super().__init__(f"{code}: {message}")
        self.code = code
        self.message = message


class ToolRegistry:
    def __init__(self, http: HttpTransport, max_infer_interval: int = 5,
                 benchmark_max_duration_s: int = 120):
        self.http = http
        self._max_interval = max_infer_interval
        self._benchmark_max = benchmark_max_duration_s

    # ---- the seven tools ----------------------------------------------------

    def get_system_metrics(self) -> dict:
        summary = self.http.get("/metrics/summary")
        scheduler = self.http.get("/scheduler/state")
        return {"metrics": summary, "scheduler": scheduler}

    def get_all_stream_status(self) -> dict:
        return self.http.get("/streams")

    def get_scheduler_state(self) -> dict:
        return self.http.get("/scheduler/state")

    def set_infer_interval(self, stream_id: str, interval: int) -> dict:
        if not isinstance(interval, int):
            raise ToolError("PARAM_INTERVAL", "interval must be an int")
        if not (0 <= interval <= self._max_interval):
            raise ToolError("PARAM_INTERVAL",
                            f"interval must be in [0,{self._max_interval}]")
        return self.http.post(f"/streams/{stream_id}/infer-interval",
                               {"interval": interval})

    def set_stream_priority(self, stream_id: str, priority: str) -> dict:
        if priority not in VALID_PRIORITIES:
            raise ToolError("PARAM_PRIORITY",
                            "priority must be 'high', 'normal' or 'low'")
        return self.http.post(f"/streams/{stream_id}/priority",
                               {"priority": priority})

    def run_benchmark(self, duration_s: int = 60,
                      per_stream: Optional[list] = None) -> dict:
        if not isinstance(duration_s, int):
            raise ToolError("PARAM_DURATION", "duration_s must be an int")
        if not (1 <= duration_s <= self._benchmark_max):
            raise ToolError("PARAM_DURATION",
                            f"duration_s must be in [1,{self._benchmark_max}]")
        body = {"duration_s": duration_s}
        if per_stream is not None:
            body["per_stream"] = per_stream
        # The server blocks the accept thread for the whole window.
        return self.http.post("/benchmark", body, timeout_s=duration_s + 20.0)

    def rollback_config(self, snapshot_id: str) -> dict:
        if not isinstance(snapshot_id, str) or not snapshot_id:
            raise ToolError("PARAM_SNAPSHOT", "snapshot_id must be a non-empty string")
        return self.http.post("/config/rollback", {"snapshot_id": snapshot_id})

    # ---- dispatch ------------------------------------------------------------

    def call(self, tool: ToolCall) -> dict:
        name, args = tool.name, tool.args
        if name == TOOL_GET_SYSTEM_METRICS:
            return self.get_system_metrics()
        if name == TOOL_GET_ALL_STREAM_STATUS:
            return self.get_all_stream_status()
        if name == TOOL_GET_SCHEDULER_STATE:
            return self.get_scheduler_state()
        if name == TOOL_SET_INFER_INTERVAL:
            return self.set_infer_interval(args.get("stream_id"), args.get("interval"))
        if name == TOOL_SET_STREAM_PRIORITY:
            return self.set_stream_priority(args.get("stream_id"), args.get("priority"))
        if name == TOOL_RUN_BENCHMARK:
            return self.run_benchmark(args.get("duration_s", 60), args.get("per_stream"))
        if name == TOOL_ROLLBACK_CONFIG:
            return self.rollback_config(args.get("snapshot_id"))
        raise ToolError("UNKNOWN_TOOL", f"unknown tool '{name}'")

    def call_tool_by_name(self, name: str, **kwargs) -> dict:
        """Direct named call for read tools / config_snapshot (executor)."""
        if name == TOOL_GET_SYSTEM_METRICS:
            return self.get_system_metrics()
        if name == TOOL_GET_ALL_STREAM_STATUS:
            return self.get_all_stream_status()
        if name == TOOL_GET_SCHEDULER_STATE:
            return self.get_scheduler_state()
        if name == "config_snapshot":
            reason = str(kwargs.get("reason", "agent"))
            data = self.http.post("/config/snapshot", {"reason": reason})
            return data
        raise ToolError("UNKNOWN_TOOL", f"unknown tool '{name}'")


# ---- JSON Schemas fed to DeepSeek function calling -------------------------

def tool_schemas(goal: dict) -> list:
    """OpenAI-compatible `tools` payload for the planner LLM.

    Descriptions carry the safety semantics the C++ layer enforces anyway:
    interval applies until the next scheduler state switch; CRITICAL blocks
    load increases; changes are bounded and audited.
    """
    streams_hint = ""
    if goal.get("cam_id"):
        streams_hint = (f" Target camera is {goal['cam_id']}: do not reduce its "
                        f"inference rate.")
    return [
        {"type": "function", "function": {
            "name": TOOL_GET_SYSTEM_METRICS,
            "description": "Per-stream FPS, frame counts, detection counts and "
                           "inference-stage latency percentiles (p50/p95/p99 ms) "
                           "plus scheduler state. Read-only.",
            "parameters": {"type": "object", "properties": {}},
        }},
        {"type": "function", "function": {
            "name": TOOL_GET_ALL_STREAM_STATUS,
            "description": "Per-stream runtime status: state, priority, "
                           "infer_interval, frames, reconnect count. Read-only.",
            "parameters": {"type": "object", "properties": {}},
        }},
        {"type": "function", "function": {
            "name": TOOL_GET_SCHEDULER_STATE,
            "description": "Scheduler state machine state and current tier "
                           "table. Read-only.",
            "parameters": {"type": "object", "properties": {}},
        }},
        {"type": "function", "function": {
            "name": TOOL_SET_INFER_INTERVAL,
            "description": "Set a stream's inference interval: 0 = every frame, "
                           "N = keep 1 frame in N. Valid range [0,5]. "
                           "Load-decreasing (raising the interval) is preferred; "
                           "reducing the interval is a load increase and is "
                           "rejected in CRITICAL state. The value applies in the "
                           "current scheduler state until the next state switch "
                           "re-asserts the policy table. Changes are "
                           "snapshotted, audited and verified. "
                           + streams_hint,
            "parameters": {"type": "object", "properties": {
                "stream_id": {"type": "string", "description": "e.g. cam4"},
                "interval": {"type": "integer", "minimum": 0, "maximum": 5},
            }, "required": ["stream_id", "interval"]},
        }},
        {"type": "function", "function": {
            "name": TOOL_SET_STREAM_PRIORITY,
            "description": "Set a stream's priority to 'high', 'normal' or "
                           "'low'. Raising priority is a load increase "
                           "(rejected in CRITICAL); lowering it is "
                           "load-decreasing. Snapshot/audit/verify as usual.",
            "parameters": {"type": "object", "properties": {
                "stream_id": {"type": "string", "description": "e.g. cam4"},
                "priority": {"type": "string", "enum": ["high", "normal", "low"]},
            }, "required": ["stream_id", "priority"]},
        }},
        {"type": "function", "function": {
            "name": TOOL_RUN_BENCHMARK,
            "description": "Run a controlled measurement window (duration_s in "
                           "[5,120], default 60). Returns per-stream and global "
                           "FPS, drop rate and latency percentiles (p50/p95/p99). "
                           "Read-only; other Control API calls queue while it runs.",
            "parameters": {"type": "object", "properties": {
                "duration_s": {"type": "integer", "minimum": 5, "maximum": 120},
            }},
        }},
        {"type": "function", "function": {
            "name": TOOL_ROLLBACK_CONFIG,
            "description": "Restore a config snapshot by snapshot_id. Use the "
                           "baseline snapshot when a change did not meet the "
                           "goal.",
            "parameters": {"type": "object", "properties": {
                "snapshot_id": {"type": "string"},
            }, "required": ["snapshot_id"]},
        }},
    ]
