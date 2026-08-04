"""Goal parsing for the single supported scenario (Stage 12).

Scenario: keep camN inference FPS >= X while reducing global P95 latency
(inference-stage latency: nvinfer sink pad -> nvtracker src pad).
Anything else is explicitly rejected (`unsupported=True`) instead of guessed.

Pure logic — no I/O, fully unit-testable.
"""

from __future__ import annotations

import re

from agent.schemas import Goal

# "cam1" / "cam 1" / "cam1 的" — capture the camera number.
_CAM_RE = re.compile(r"cam\s*(\d+)", re.IGNORECASE)

# FPS floor: "fps >= 15", "fps不低于15", "至少 15 fps", "不低于15帧".
# The digit block must not be preceded by a letter — this keeps "P95" from
# being parsed as a FPS value ("...P95 延迟" must not yield min_fps=95).
_FPS_SUFFIX = r"(?:>=|≥|不低于|至少|至少为|no less than|at least)?\s*(?<![A-Za-z])(\d+(?:\.\d+)?)"
_FPS_RE = re.compile(
    r"(?:fps|FPS|帧率|帧速|f/s)\D{0,30}" + _FPS_SUFFIX
    + r"|" + _FPS_SUFFIX + r"\s*(?:fps|FPS|帧率|帧速)",
    re.IGNORECASE,
)

# Latency intent: goal must mention reducing/ensuring P95/latency/延迟.
_P95_RE = re.compile(r"(p95|p99|延迟|时延|latency|延时)", re.IGNORECASE)

_DEFAULT_MIN_FPS = 15.0


def parse_goal(text: str, default_min_fps: float = _DEFAULT_MIN_FPS) -> Goal:
    """Parse the fixed scenario.  Unsupported text yields Goal(unsupported=True)."""
    cam_m = _CAM_RE.search(text)
    if not cam_m:
        return _unsupported(text, "no camera constraint (camN) found")
    cam_id = f"cam{cam_m.group(1)}"

    if not _P95_RE.search(text):
        return _unsupported(text, "no latency/P95 intent found")

    min_fps = default_min_fps
    fps_m = _FPS_RE.search(text)
    if fps_m:
        # Two alternation branches → the digits may sit in group 1 or 2.
        raw = fps_m.group(1) if fps_m.group(1) is not None else fps_m.group(2)
        try:
            parsed = float(raw)
        except (ValueError, TypeError):
            return _unsupported(text, "unparseable FPS value")
        if parsed <= 0.0:
            return _unsupported(text, "FPS floor must be positive")
        min_fps = parsed
    return Goal(goal_text=text, cam_id=cam_id, min_fps=min_fps)


def _unsupported(text: str, why: str) -> Goal:
    return Goal(goal_text=text, unsupported=True, unsupported_reason=why)
