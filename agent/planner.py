"""Candidate planning: LLM first, deterministic fallback (Stage 12).

Hybrid decision layer (README §17: the LLM only generates low-frequency
candidate plans over deterministic controls).  Every LLM candidate is
cleaned before execution: whitelisted tool, ≤ max_actions actions, at most
max_actions distinct streams, interval/priority values clamped to safe
ranges, and the goal camera is never de-prioritized.

The deterministic fallback (used when the LLM is unavailable, breaks the
circuit, returns nothing, or produces only invalid candidates) is strictly
load-decreasing: pick low-priority streams with interval == 0 and raise the
interval by one step, at most max_actions streams, never the goal camera.
"""

from __future__ import annotations

from typing import Optional

from agent.schemas import (
    TOOL_GET_ALL_STREAM_STATUS,
    TOOL_SET_INFER_INTERVAL,
    TOOL_SET_STREAM_PRIORITY,
    Plan,
    ToolCall,
)


class Planner:
    def __init__(self, cfg: dict):
        self.cfg = cfg
        self.max_actions = int(cfg.get("max_actions_per_round", 2))
        self.max_interval = int(cfg.get("max_infer_interval", 5))

    # ---- LLM candidates ------------------------------------------------------

    def clean_llm_candidates(self, calls: list, goal) -> list:
        """Validate and clamp LLM tool calls; drop anything unsafe."""
        out = []
        streams_touched = set()
        for call in calls:
            if not isinstance(call, ToolCall):
                continue
            if len(out) >= self.max_actions:
                break
            name = call.name
            args = dict(call.args or {})
            if name == TOOL_SET_INFER_INTERVAL:
                sid = str(args.get("stream_id", ""))
                if not sid:
                    continue
                try:
                    interval = int(args["interval"])
                except (KeyError, TypeError, ValueError):
                    continue
                interval = max(0, min(self.max_interval, interval))
                if sid == goal.cam_id and interval > 0:
                    # Never slow the goal camera's inference rate.
                    continue
                if sid in streams_touched:
                    continue
                streams_touched.add(sid)
                out.append(ToolCall(name, {"stream_id": sid, "interval": interval}))
            elif name == TOOL_SET_STREAM_PRIORITY:
                sid = str(args.get("stream_id", ""))
                priority = str(args.get("priority", ""))
                if not sid or priority not in ("high", "normal", "low"):
                    continue
                if sid == goal.cam_id and priority != "high":
                    continue
                if sid in streams_touched:
                    continue
                streams_touched.add(sid)
                out.append(ToolCall(name, {"stream_id": sid, "priority": priority}))
            # run_benchmark / rollback_config / read tools are handled by the
            # executor itself; they are not part of the change plan.
        return out

    # ---- deterministic fallback ----------------------------------------------

    def deterministic_plan(self, observation: dict, goal, interval_step: int = 1) -> Plan:
        """Strictly load-decreasing: raise the interval of the lowest-priority
        interval==0 streams (never the goal camera).  `interval_step` is the
        target interval for round N (1, 2, ...)."""
        statuses = []
        if isinstance(observation, dict):
            streams_data = observation.get(TOOL_GET_ALL_STREAM_STATUS, {})
            if isinstance(streams_data, dict):
                statuses = streams_data.get("streams", [])
        candidates = []
        for st in statuses:
            sid = st.get("stream_id", "")
            if sid == goal.cam_id:
                continue
            if st.get("infer_interval", 0) == 0:
                rank = {"low": 0, "normal": 1, "high": 2}.get(st.get("priority"), 2)
                candidates.append((rank, sid))
        candidates.sort()
        step = max(1, min(self.max_interval, interval_step))
        actions = [
            ToolCall(TOOL_SET_INFER_INTERVAL,
                     {"stream_id": sid, "interval": step})
            for _, sid in candidates[: self.max_actions]
        ]
        rationale = ("deterministic fallback: raise infer_interval to "
                     f"{step} on {len(actions)} non-goal stream(s) with interval 0")
        return Plan(actions=actions, source="deterministic", rationale=rationale)

    # ---- entry point -----------------------------------------------------------

    def plan(self, observation: dict, goal, llm_calls: Optional[list],
             attempt: int = 1) -> Plan:
        """llm_calls is None when the LLM is unavailable; otherwise a list of
        raw ToolCall candidates (possibly empty).  `attempt` selects the
        deterministic escalation step (1, 2, ...)."""
        if llm_calls is not None:
            cleaned = self.clean_llm_candidates(llm_calls, goal)
            if cleaned:
                return Plan(actions=cleaned, source="llm",
                            rationale="LLM candidate plan after cleaning")
        return self.deterministic_plan(observation, goal, attempt)
