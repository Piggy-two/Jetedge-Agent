"""Goal-driven execution loop (Stage 12, CLAUDE.md §16 + implementation_plan
§65):

    observe → CRITICAL pre-check → baseline snapshot → before benchmark
    → plan (LLM candidates or deterministic fallback) → bounded change
    → after benchmark → verify with real metrics → keep or roll back
    → audit + report

Guarantees:
  * zero write operations while the scheduler is CRITICAL;
  * every write re-checks the scheduler state immediately before the call
    (the server independently rejects load increases in CRITICAL);
  * a hard deadline aborts the run and rolls back to the baseline;
  * the LLM never decides the verdict — validator does;
  * the agent runs as a separate process: killing it never touches the
    pipeline (the C++ server owns all state).
"""

from __future__ import annotations

import time
from typing import Optional

from agent.audit import AgentAudit, write_report
from agent.benchmark import run_benchmark
from agent.deepseek_client import DeepSeekError
from agent.http_client import ControlApiError
from agent.planner import Planner
from agent.rollback import capture_baseline_streams, rollback_to
from agent.schemas import (
    TOOL_GET_ALL_STREAM_STATUS,
    TOOL_GET_SCHEDULER_STATE,
    TOOL_GET_SYSTEM_METRICS,
    Plan,
    VerificationResult,
)
from agent.tool_registry import ToolError, ToolRegistry
from agent.validator import ValidationError, check_goal_met


class RunAborted(Exception):
    """Run-level abort (deadline / CRITICAL / unusable data)."""

    def __init__(self, reason: str):
        super().__init__(reason)
        self.reason = reason


class Executor:
    def __init__(self, cfg: dict, tools: ToolRegistry, planner: Planner,
                 audit: AgentAudit, llm=None):
        self.cfg = cfg
        self.tools = tools
        self.planner = planner
        self.audit = audit
        self.llm = llm  # optional DeepSeekClient; None → deterministic only
        self.deadline_s = float(cfg.get("deadline_s", 600))
        self.deadline_at = 0.0
        self.max_rounds = int(cfg.get("max_change_rounds", 2))

    # ---- helpers ---------------------------------------------------------------

    def _check_deadline(self, phase: str):
        if time.monotonic() >= self.deadline_at:
            raise RunAborted(f"deadline exceeded during {phase}")

    def _observe(self) -> dict:
        """Read-only snapshot of the system, with a bounded timeout."""
        deadline = time.monotonic() + float(self.cfg.get("observe_timeout_s", 15.0))
        observation = {}
        for tool_name in (TOOL_GET_ALL_STREAM_STATUS, TOOL_GET_SYSTEM_METRICS,
                          TOOL_GET_SCHEDULER_STATE):
            while True:
                self._check_deadline("observe")
                if time.monotonic() >= deadline:
                    raise RunAborted("observe timed out")
                try:
                    observation[tool_name] = self.tools.call_tool_by_name(tool_name)
                    break
                except (ControlApiError, ToolError) as exc:
                    self.audit.log("observe_retry", False, tool=tool_name,
                                   error_code=exc.code if hasattr(exc, "code") else "",
                                   note=str(exc))
                    time.sleep(1.0)
        return observation

    def _scheduler_critical(self, observation: dict) -> bool:
        sched = observation.get(TOOL_GET_SCHEDULER_STATE, {})
        return str(sched.get("state", "")) == "CRITICAL"

    def _run_bounded_change(self, plan: Plan, goal) -> int:
        """Apply the plan's write actions (≤ max_actions), skipping anything
        unsafe right before the call.  Returns the number applied."""
        applied = 0
        for action in plan.actions:
            self._check_deadline("change")
            # Re-check the scheduler immediately before each write.
            try:
                sched = self.tools.call_tool_by_name(TOOL_GET_SCHEDULER_STATE)
            except (ControlApiError, ToolError) as exc:
                self.audit.log("scheduler_check", False, note=str(exc))
                continue
            if str(sched.get("state", "")) == "CRITICAL":
                self.audit.log("action_skipped_critical", True,
                               tool=action.name, args=action.args)
                continue
            try:
                self.tools.call(action)
                applied += 1
                self.audit.log("action", True, tool=action.name, args=action.args)
            except (ControlApiError, ToolError) as exc:
                self.audit.log("action", False, tool=action.name, args=action.args,
                               error_code=getattr(exc, "code", ""), note=str(exc))
        return applied

    # ---- the loop ---------------------------------------------------------------

    def run(self, goal) -> VerificationResult:
        self.deadline_at = time.monotonic() + self.deadline_s
        report_lines = [f"- 目标: `{goal.goal_text}`", "- 开始: "
                        + time.strftime("%Y-%m-%d %H:%M:%S")]
        baseline_id = ""
        baseline_streams = {}
        try:
            # 1. Observe.
            observation = self._observe()
            if self._scheduler_critical(observation):
                raise RunAborted("scheduler is CRITICAL — no write ops allowed")
            report_lines.append("- 观察: 4 路流状态 + 指标 + 调度器状态 已读取")

            # 2. Baseline snapshot (rollback target).
            baseline_streams = capture_baseline_streams(
                observation.get(TOOL_GET_ALL_STREAM_STATUS, {}))
            snap = self.tools.call_tool_by_name("config_snapshot",
                                                reason=f"agent_baseline:{self.audit.run_id}")
            baseline_id = snap.get("snapshot_id", "")
            self.audit.log("baseline_snapshot", True, snapshot_id=baseline_id)
            report_lines.append(f"- 基线快照: `{baseline_id}`")

            # 3. Before benchmark.
            duration = int(self.cfg.get("benchmark_duration_s", 60))
            before = self._run_benchmark(duration, "before")
            report_lines.append(
                f"- before 基准窗口 {duration}s: 全局 P95 = "
                f"{_fmt_p95(before.global_p95_ms)}, 全局 FPS = "
                f"{before.global_input_fps:.1f}, drop = "
                f"{before.global_drop_rate * 100:.2f}%")

            # 4. Plan (LLM candidates or deterministic fallback).
            observation = self._observe()
            llm_calls = None
            llm_note = "LLM 不可用 → 确定性默认策略"
            if self.llm is not None and self.llm.available:
                try:
                    llm_calls = self._ask_llm(observation, goal, before)
                    self.audit.log("llm_candidates", True,
                                   calls=[{"name": c.name, "args": c.args}
                                          for c in llm_calls])
                    llm_note = (f"LLM 候选 {len(llm_calls)} 条"
                                if llm_calls else "LLM 未给出候选 → 确定性默认策略")
                except (DeepSeekError, ControlApiError) as exc:
                    llm_note = f"LLM 失败({exc}) → 确定性默认策略"
            plan = self.planner.plan(observation, goal, llm_calls)
            self.audit.log("plan", True, source=plan.source, note=plan.rationale)
            report_lines.append(f"- 计划来源: {plan.source} — {plan.rationale} ({llm_note})")

            # 5. Bounded change + verify loop.
            for attempt in range(1, self.max_rounds + 1):
                self._check_deadline(f"round {attempt}")
                applied = self._run_bounded_change(plan, goal)
                if applied == 0 and not plan.actions:
                    report_lines.append("- 无变更动作(计划为空)")
                    break
                after = self._run_benchmark(duration, f"after_round{attempt}")
                try:
                    verdict = check_goal_met(before, after, goal)
                except ValidationError as exc:
                    raise RunAborted(str(exc))
                self.audit.log("verify", verdict.ok, round=attempt,
                               before_p95=before.global_p95_ms,
                               after_p95=after.global_p95_ms,
                               reasons=verdict.reasons)
                report_lines.append(
                    f"- round {attempt}: after P95 = {_fmt_p95(after.global_p95_ms)} ms "
                    f"→ {'达标' if verdict.ok else '不达标'}: {'; '.join(verdict.reasons) or 'OK'}")
                if verdict.ok:
                    report_lines.append(f"- **结果: 保留配置(达标)** — "
                                        f"退出码 0")
                    return self._finish(goal, before, after, baseline_id, 0,
                                        report_lines, "保留")
                # Not met: roll back, then retry once with the next step
                # (deterministic escalation: interval+1 on the same streams).
                rb = rollback_to(self.tools.http, baseline_id, baseline_streams)
                self.audit.log("rollback", rb.ok, snapshot_id=baseline_id,
                               note=rb.reason)
                report_lines.append(f"- round {attempt} 回滚: {'成功' if rb.ok else '失败: ' + rb.reason}")
                if not rb.ok:
                    raise RunAborted(f"rollback failed: {rb.reason}")
                if attempt == self.max_rounds:
                    break
                observation = self._observe()
                plan = self.planner.plan(observation, goal, None,
                                         attempt=attempt + 1)

            report_lines.append(f"- **结果: 未达标, 已回滚基线 `{baseline_id}`** — 退出码 1")
            return self._finish(goal, before, None, baseline_id, 1, report_lines,
                                "回滚")
        except RunAborted as exc:
            self.audit.log("abort", False, note=exc.reason)
            report_lines.append(f"- **中止: {exc.reason}**")
            if baseline_id and baseline_streams:
                rb = rollback_to(self.tools.http, baseline_id, baseline_streams)
                report_lines.append(f"- 中止回滚: {'成功' if rb.ok else '失败: ' + rb.reason}")
            return self._finish(goal, None, None, baseline_id, 1, report_lines,
                                "中止回滚")

    # ---- pieces -----------------------------------------------------------------

    def _ask_llm(self, observation: dict, goal, before) -> list:
        from agent.tool_registry import tool_schemas

        obs_text = _observation_text(observation, before, goal)
        prompt_path = self.cfg.get("system_prompt_path", "agent/prompts/system_prompt.md")
        try:
            with open(prompt_path, "r", encoding="utf-8") as fh:
                system_prompt = fh.read()
        except OSError:
            system_prompt = (
                "You are the planning layer of a safe edge AI control agent. "
                "Propose at most 2 low-risk configuration changes that reduce "
                "global P95 latency while keeping the goal camera's inference "
                "FPS above its floor. Prefer raising infer_interval on "
                "low-priority streams. Never increase load in CRITICAL state. "
                "Return tool calls only.")
        return self.llm.plan_candidates(system_prompt, obs_text,
                                        tool_schemas({"cam_id": goal.cam_id}))

    def _run_benchmark(self, duration_s: int, phase: str):
        self._check_deadline(phase)
        try:
            result = run_benchmark(self.tools.http, duration_s)
        except ControlApiError as exc:
            raise RunAborted(f"{phase} benchmark failed: {exc}")
        self.audit.log("benchmark", True, window=phase, duration_s=duration_s,
                       global_p95_ms=result.global_p95_ms)
        return result

    def _finish(self, goal, before, after, baseline_id, exit_code, report_lines,
                outcome: str) -> VerificationResult:
        report_lines.append(f"- 退出码: {exit_code}")
        report_path = self.cfg.get("report_dir", "logs/agent/reports") + \
            f"/{self.audit.run_id}.md"
        write_report(report_path, self.audit.run_id, report_lines)
        self.audit.log("run_end", exit_code == 0, exit_code=exit_code,
                       outcome=outcome, baseline_snapshot=baseline_id,
                       report=report_path)
        return VerificationResult(ok=exit_code == 0, reasons=[])


def _fmt_p95(value) -> str:
    return f"{value:.1f}" if value is not None else "N/A"


def _observation_text(observation: dict, before, goal) -> str:
    lines = [f"目标: {goal.goal_text}",
             f"目标相机: {goal.cam_id}, 推理 FPS 保底: {goal.min_fps}",
             f"before 基准: 全局P95={_fmt_p95(before.global_p95_ms)} ms "
             f"FPS={before.global_input_fps:.1f} drop={before.global_drop_rate * 100:.2f}%"]
    statuses = observation.get(TOOL_GET_ALL_STREAM_STATUS, {}).get("streams", [])
    for st in statuses:
        lines.append(f"流 {st.get('stream_id')}: state={st.get('state')} "
                     f"priority={st.get('priority')} interval={st.get('infer_interval')}")
    sched = observation.get(TOOL_GET_SCHEDULER_STATE, {})
    lines.append(f"调度器: {sched.get('state')} "
                 f"table={{{sched.get('table_high')},{sched.get('table_normal')},{sched.get('table_low')}}}")
    return "\n".join(lines)
