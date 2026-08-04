"""Benchmark response parsing (Stage 12).

Converts the POST /benchmark JSON response into BenchmarkResult.  Pure
parsing — numbers come from the server exactly as measured.
"""

from __future__ import annotations

from agent.http_client import HttpTransport
from agent.schemas import BenchmarkResult, StreamBench


def parse_benchmark(data: dict) -> BenchmarkResult:
    result = BenchmarkResult(
        duration_s=int(data.get("duration_s", 0)),
        elapsed_ms=int(data.get("elapsed_ms", 0)),
        scheduler_state_before=str(data.get("scheduler_state_before", "")),
        scheduler_state_after=str(data.get("scheduler_state_after", "")),
    )
    global_ = data.get("global") or {}
    result.global_p95_ms = _as_float(global_.get("p95_ms"))
    result.global_avg_ms = _as_float(global_.get("avg_ms"))
    result.global_drop_rate = float(global_.get("drop_rate", 0.0) or 0.0)
    result.global_input_fps = float(global_.get("input_fps", 0.0) or 0.0)

    streams = data.get("streams") or {}
    for sid, s in streams.items():
        lat = s.get("latency") or {}
        result.streams[str(sid)] = StreamBench(
            frames_in=int(s.get("frames_in", 0)),
            frames_out=int(s.get("frames_out", 0)),
            input_fps=float(s.get("input_fps", 0.0) or 0.0),
            output_fps=float(s.get("output_fps", 0.0) or 0.0),
            drop_rate=float(s.get("drop_rate", 0.0) or 0.0),
            complete=bool(s.get("complete", False)),
            lat_avg_ms=_as_float(lat.get("avg_ms")),
            lat_p50_ms=_as_float(lat.get("p50_ms")),
            lat_p95_ms=_as_float(lat.get("p95_ms")),
            lat_p99_ms=_as_float(lat.get("p99_ms")),
        )
    return result


def run_benchmark(http: HttpTransport, duration_s: int) -> BenchmarkResult:
    data = http.post("/benchmark", {"duration_s": duration_s},
                     timeout_s=duration_s + 20.0)
    return parse_benchmark(data)


def _as_float(value):
    if value is None:
        return None
    try:
        return float(value)
    except (TypeError, ValueError):
        return None
