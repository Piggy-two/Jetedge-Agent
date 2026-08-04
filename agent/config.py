"""Agent runtime configuration (Stage 12).

Defaults mirror configs/streams_stage11.yaml (control on 127.0.0.1:8090) and
src/llm/llm_config.h timeout/retry/circuit-breaker parameters.  A YAML file
may override everything here (agent/config.yaml).
"""

from __future__ import annotations

import os

# Control API (loopback only — the server never binds externally).
CONTROL_BASE_URL = os.environ.get("JETEDGE_CONTROL_URL", "http://127.0.0.1:8090")

# Defaults, overridable via agent/config.yaml.
DEFAULTS = {
    # --- goal ----------------------------------------------------------------
    "default_min_fps": 15.0,
    "p95_reduction_ratio": 0.15,
    "p95_reduction_abs_ms": 8.0,
    "p95_trivial_ms": 20.0,
    "fps_tolerance_ratio": 0.05,
    "drop_rate_tolerance_pp": 0.02,
    "drop_rate_floor": 0.01,
    # --- execution loop ------------------------------------------------------
    "observe_timeout_s": 15.0,
    "benchmark_duration_s": 60,
    "benchmark_min_duration_s": 5,   # server-side floor
    "benchmark_max_duration_s": 120,  # server-side ceiling
    "max_change_rounds": 2,
    "max_actions_per_round": 2,
    "deadline_s": 600,
    # --- deepseek (mirrors llm_config.h: timeout 20s, retries 2) ------------
    "deepseek_endpoint": "https://api.deepseek.com/v1/chat/completions",
    "deepseek_model": "deepseek-v4-flash",
    "deepseek_timeout_s": 20.0,
    "deepseek_max_retries": 2,
    "deepseek_max_tokens": 512,
    # circuit breaker: 5 consecutive failures -> OPEN 30s -> HALF_OPEN, 2
    # successes back to CLOSED (mirrors llm_config.h defaults).
    "breaker_fail_threshold": 5,
    "breaker_reset_s": 30.0,
    "breaker_half_open_success": 2,
    # --- secrets --------------------------------------------------------------
    "secrets_file": os.path.expanduser("~/.jetedge/secrets.env"),
    # --- reporting ------------------------------------------------------------
    "report_dir": "logs/agent/reports",
    "audit_file": "logs/agent/audit.jsonl",
}


def load_config(path: str) -> dict:
    """Merge YAML overrides (if any) over DEFAULTS.  Returns a plain dict."""
    cfg = dict(DEFAULTS)
    if not path:
        return cfg
    try:
        import yaml  # type: ignore
    except ImportError:
        return cfg  # no YAML available: defaults only
    try:
        with open(path, "r", encoding="utf-8") as fh:
            overrides = yaml.safe_load(fh) or {}
    except (OSError, ValueError):
        return cfg
    if isinstance(overrides, dict):
        for key, value in overrides.items():
            if key in cfg:
                cfg[key] = value
    return cfg
