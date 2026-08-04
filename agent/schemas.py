"""Agent data structures and constants (Stage 12).

Plain dataclasses only — no HTTP, no I/O.  Kept importable by every other
agent module and by the unit tests.
"""

from __future__ import annotations

import dataclasses
from dataclasses import dataclass, field
from typing import Optional


# ---------------------------------------------------------------------------
# Goal
# ---------------------------------------------------------------------------

@dataclass(frozen=True)
class Goal:
    """The single supported scenario:

    keep `cam_id` inference FPS >= min_fps while reducing global P95 latency
    (inference-stage latency: nvinfer sink pad -> nvtracker src pad).
    """

    goal_text: str
    cam_id: str = "cam1"
    min_fps: float = 15.0
    # Goal is met when after.p95 <= before.p95 * (1 - ratio) AND
    # after.p95 <= before.p95 - abs_ms (dual threshold vs measurement noise).
    p95_reduction_ratio: float = 0.15
    p95_reduction_abs_ms: float = 8.0
    # Below this the goal counts as already met — no change needed.
    p95_trivial_ms: float = 20.0
    # cam1 FPS tolerance: noise guard, 5% below the stated floor.
    fps_tolerance_ratio: float = 0.05
    # drop-rate must not worsen by more than this many percentage points.
    drop_rate_tolerance_pp: float = 0.02
    drop_rate_floor: float = 0.01
    unsupported: bool = False
    unsupported_reason: str = ""


@dataclass(frozen=True)
class ToolCall:
    name: str
    args: dict = field(default_factory=dict)


@dataclass(frozen=True)
class Plan:
    actions: list = field(default_factory=list)  # list[ToolCall]
    source: str = "deterministic"  # "llm" | "deterministic"
    rationale: str = ""


# ---------------------------------------------------------------------------
# Benchmark / verification results
# ---------------------------------------------------------------------------

@dataclass
class StreamBench:
    frames_in: int = 0
    frames_out: int = 0
    input_fps: float = 0.0
    output_fps: float = 0.0
    drop_rate: float = 0.0
    complete: bool = False
    # latency fields (ms); None when no samples were collected
    lat_avg_ms: Optional[float] = None
    lat_p50_ms: Optional[float] = None
    lat_p95_ms: Optional[float] = None
    lat_p99_ms: Optional[float] = None


@dataclass
class BenchmarkResult:
    duration_s: int = 0
    elapsed_ms: int = 0
    scheduler_state_before: str = ""
    scheduler_state_after: str = ""
    streams: dict = field(default_factory=dict)  # stream_id -> StreamBench
    global_p95_ms: Optional[float] = None
    global_avg_ms: Optional[float] = None
    global_drop_rate: float = 0.0
    global_input_fps: float = 0.0

    def stream(self, stream_id: str) -> Optional[StreamBench]:
        return self.streams.get(stream_id)


@dataclass
class VerificationResult:
    ok: bool
    reasons: list = field(default_factory=list)  # human-readable
    already_met: bool = False


# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------

# Control API envelope keys.
ENVELOPE_SUCCESS = "success"
ENVELOPE_DATA = "data"
ENVELOPE_ERROR_CODE = "error_code"
ENVELOPE_ERROR = "error"

# Tool names (implementation_plan §64).
TOOL_GET_SYSTEM_METRICS = "get_system_metrics"
TOOL_GET_ALL_STREAM_STATUS = "get_all_stream_status"
TOOL_GET_SCHEDULER_STATE = "get_scheduler_state"
TOOL_SET_INFER_INTERVAL = "set_infer_interval"
TOOL_SET_STREAM_PRIORITY = "set_stream_priority"
TOOL_RUN_BENCHMARK = "run_benchmark"
TOOL_ROLLBACK_CONFIG = "rollback_config"

ALL_TOOLS = [
    TOOL_GET_SYSTEM_METRICS,
    TOOL_GET_ALL_STREAM_STATUS,
    TOOL_GET_SCHEDULER_STATE,
    TOOL_SET_INFER_INTERVAL,
    TOOL_SET_STREAM_PRIORITY,
    TOOL_RUN_BENCHMARK,
    TOOL_ROLLBACK_CONFIG,
]

# Load-increasing direction flags (used by planner and validator).
PRIORITY_RANK = {"low": 0, "normal": 1, "high": 2}
