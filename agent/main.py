#!/usr/bin/env python3
"""JetEdge-Agent Stage 12 — safe goal-driven control agent (single run).

Usage:
    python3 agent/main.py --goal "保证 cam1 推理 FPS 不低于 15,降低全局 P95 延迟"
                          [--config agent/config.yaml] [--no-llm]

Exit codes:
    0  goal met, change kept
    1  goal not met (auto-rollback) or run aborted
    2  configuration / goal parse error
    3  Control API unreachable at startup

The agent is a separate process: it talks to the Control API over loopback
HTTP only.  Killing it never affects the pipeline (the C++ server owns all
state).  The real-time pipeline does not depend on this process.
"""

from __future__ import annotations

import argparse
import os
import sys
import time

# Make `python3 agent/main.py` work from the repo root: the script's parent
# dir must be importable as the `agent` package (python3 -m agent.main works
# without this, but direct invocation sets sys.path[0] to agent/).
sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from agent.audit import AgentAudit
from agent.config import CONTROL_BASE_URL, DEFAULTS, load_config
from agent.deepseek_client import DeepSeekClient, load_api_key
from agent.executor import Executor
from agent.goal_parser import parse_goal
from agent.http_client import ControlApiError, HttpTransport
from agent.planner import Planner
from agent.tool_registry import ToolRegistry


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description="JetEdge-Agent Stage 12 goal loop")
    parser.add_argument("--goal", required=True, help="自然语言目标(当前仅支持单场景)")
    parser.add_argument("--config", default="agent/config.yaml",
                        help="agent 配置文件(YAML)")
    parser.add_argument("--no-llm", action="store_true",
                        help="禁用 LLM 规划,只用确定性默认策略")
    parser.add_argument("--base-url", default=None,
                        help=f"Control API 地址(默认 {CONTROL_BASE_URL})")
    parser.add_argument("--benchmark-duration", type=int, default=None,
                        help="测量窗口秒数(覆盖 config.yaml, 5-120)")
    args = parser.parse_args(argv)

    cfg = load_config(args.config)
    if args.benchmark_duration is not None:
        if not (5 <= args.benchmark_duration <= 120):
            print("--benchmark-duration 必须在 [5,120]", file=sys.stderr)
            return 2
        cfg["benchmark_duration_s"] = args.benchmark_duration
    goal = parse_goal(args.goal, float(cfg.get("default_min_fps", 15.0)))
    if goal.unsupported:
        print(f"不支持的目标: {goal.unsupported_reason}", file=sys.stderr)
        return 2

    run_id = f"run_{int(time.time() * 1000)}"
    audit = AgentAudit(cfg.get("audit_file", "logs/agent/audit.jsonl"), run_id)
    audit.log("start", True, goal=args.goal, no_llm=bool(args.no_llm))

    base_url = args.base_url or CONTROL_BASE_URL
    http = HttpTransport(base_url, timeout_s=float(cfg.get("observe_timeout_s", 15.0)))

    # Startup reachability check (exit 3 without touching anything).
    try:
        http.get("/health", timeout_s=5.0)
    except ControlApiError as exc:
        audit.log("startup", False, note=f"control api unreachable: {exc}")
        audit.close()
        print(f"Control API 不可达: {exc}", file=sys.stderr)
        return 3

    tools = ToolRegistry(
        http,
        max_infer_interval=int(cfg.get("max_infer_interval", 5)),
        benchmark_max_duration_s=int(cfg.get("benchmark_max_duration_s", 120)),
    )
    planner = Planner(cfg)

    llm = None
    if not args.no_llm:
        api_key = load_api_key(cfg)
        if api_key:
            llm = DeepSeekClient(cfg, api_key=api_key)
        else:
            audit.log("llm", False, note="no DEEPSEEK_API_KEY — deterministic mode")
            print("未找到 DEEPSEEK_API_KEY,使用确定性默认策略", file=sys.stderr)

    executor = Executor(cfg, tools, planner, audit, llm=llm)
    result = executor.run(goal)
    audit.close()
    if result.ok:
        print(f"完成: 达标, 配置保留 (run {run_id})")
    else:
        print(f"完成: 未达标/中止, 已回滚 (run {run_id})")
    return 0 if result.ok else 1


if __name__ == "__main__":
    sys.exit(main())
