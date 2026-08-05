"""DeepSeek function-calling client with circuit breaker (Stage 12).

The LLM only *proposes* candidate plans; the executor verifies every result
with real metrics and rolls back when the goal is not met.  A broken or
absent LLM degrades to the deterministic default planner — never to a guess.

The circuit breaker mirrors src/llm/llm_config.h semantics:
  CLOSED (successes) -> fail_threshold consecutive failures -> OPEN
  OPEN waits reset_s -> HALF_OPEN -> half_open_success successes -> CLOSED
While OPEN, plan() returns an empty candidate list without a network call.

API key handling: read from ~/.jetedge/secrets.env (DEEPSEEK_API_KEY=...) or
the JETEDGE_DEEPSEEK_API_KEY environment variable.  The key is never logged,
never written to reports, never part of any output.
"""

from __future__ import annotations

import json
import os
import re
import time
from typing import Optional

import urllib.error
import urllib.request

from agent.schemas import ToolCall

_MARKDOWN_FENCE_RE = re.compile(r"^```(?:json)?\s*|\s*```$", re.IGNORECASE)


def read_secrets_file(path: str) -> dict:
    """Parse KEY=VALUE lines; '#' comments and blanks are skipped."""
    secrets = {}
    try:
        with open(path, "r", encoding="utf-8") as fh:
            for raw in fh:
                line = raw.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                key, _, value = line.partition("=")
                secrets[key.strip()] = value.strip().strip('"').strip("'")
    except OSError:
        return {}
    return secrets


class DeepSeekError(Exception):
    pass


class CircuitBreaker:
    def __init__(self, fail_threshold: int = 5, reset_s: float = 30.0,
                 half_open_success: int = 2, clock=time.monotonic):
        self.fail_threshold = fail_threshold
        self.reset_s = reset_s
        self.half_open_success = half_open_success
        self._clock = clock
        self.state = "CLOSED"  # CLOSED | OPEN | HALF_OPEN
        self._consecutive_failures = 0
        self._opened_at = 0.0
        self._half_open_successes = 0

    def allow(self) -> bool:
        if self.state == "OPEN":
            if self._clock() - self._opened_at >= self.reset_s:
                self.state = "HALF_OPEN"
                self._half_open_successes = 0
            else:
                return False
        return True

    def record_success(self):
        self._consecutive_failures = 0
        if self.state == "HALF_OPEN":
            self._half_open_successes += 1
            if self._half_open_successes >= self.half_open_success:
                self.state = "CLOSED"

    def record_failure(self):
        self._consecutive_failures += 1
        if self.state == "HALF_OPEN":
            self.state = "OPEN"
            self._opened_at = self._clock()
            self._half_open_successes = 0
        elif self._consecutive_failures >= self.fail_threshold:
            self.state = "OPEN"
            self._opened_at = self._clock()

    def state_name(self) -> str:
        return self.state


class DeepSeekClient:
    def __init__(self, cfg: dict, api_key: str = "", clock=time.monotonic):
        self.cfg = cfg
        self.api_key = api_key
        self.breaker = CircuitBreaker(
            fail_threshold=cfg.get("breaker_fail_threshold", 5),
            reset_s=cfg.get("breaker_reset_s", 30.0),
            half_open_success=cfg.get("breaker_half_open_success", 2),
            clock=clock,
        )
        self._clock = clock

    @property
    def available(self) -> bool:
        return bool(self.api_key)

    def plan_candidates(self, system_prompt: str, observation: str,
                        tools: list) -> list:
        """Ask the LLM for candidate tool sequences.  Returns a flat list of
        ToolCall (possibly empty).  Raises DeepSeekError on transport
        failures; returns [] when the model returned no tool calls."""
        if not self.breaker.allow():
            raise DeepSeekError("circuit open")
        payload = {
            "model": self.cfg.get("deepseek_model", "deepseek-v4-flash"),
            "temperature": 0,
            "max_tokens": self.cfg.get("deepseek_max_tokens", 512),
            # deepseek-v4-flash reasons by default; with a bounded budget the
            # reasoning consumes it and tool_calls/content come back empty
            # (verified 2026-08-05, same fix as src/llm/prompt_manager.cpp).
            # Routine planning is non-thinking by design (CLAUDE.md §14).
            "thinking": {"type": "disabled"},
            "messages": [
                {"role": "system", "content": system_prompt},
                {"role": "user", "content": observation},
            ],
            "tools": tools,
            "tool_choice": "auto",
        }
        raw = self._post_chat(payload)
        self.breaker.record_success()
        calls = []
        for choice in raw.get("choices", []):
            message = choice.get("message", {})
            for tc in message.get("tool_calls", []):
                fn = tc.get("function", {})
                name = fn.get("name", "")
                args = _parse_arguments(fn.get("arguments", ""))
                if name:
                    calls.append(ToolCall(name=name, args=args))
        return calls

    def _post_chat(self, payload: dict) -> dict:
        body = json.dumps(payload).encode("utf-8")
        req = urllib.request.Request(
            self.cfg["deepseek_endpoint"], data=body, method="POST",
            headers={
                "Content-Type": "application/json",
                "Authorization": f"Bearer {self.api_key}",
            },
        )
        last_error = None
        for attempt in range(self.cfg.get("deepseek_max_retries", 2) + 1):
            if attempt > 0:
                time.sleep(2.0 * (2 ** (attempt - 1)))  # 2s, 4s backoff
            try:
                with urllib.request.urlopen(
                        req, timeout=self.cfg.get("deepseek_timeout_s", 20.0)) as resp:  # nosec
                    raw = resp.read().decode("utf-8", errors="replace")
                data = json.loads(raw)
                if not isinstance(data, dict):
                    raise DeepSeekError("response is not an object")
                return data
            except urllib.error.HTTPError as exc:
                last_error = DeepSeekError(f"http {exc.code}")
                if 400 <= exc.code < 500:
                    break  # client errors are not transient — do not retry
            except (urllib.error.URLError, TimeoutError, OSError, json.JSONDecodeError) as exc:
                last_error = DeepSeekError(str(exc))
        self.breaker.record_failure()
        raise last_error if last_error else DeepSeekError("no response")


def _parse_arguments(raw: str) -> dict:
    """Parse tool-call arguments JSON, stripping markdown fences defensively
    (the project hit fenced JSON on Qwen; DeepSeek gets the same guard)."""
    if not isinstance(raw, str):
        return {}
    cleaned = _MARKDOWN_FENCE_RE.sub("", raw).strip()
    if not cleaned:
        return {}
    try:
        parsed = json.loads(cleaned)
    except json.JSONDecodeError:
        return {}
    return parsed if isinstance(parsed, dict) else {}


def load_api_key(cfg: dict, env_key: str = "JETEDGE_DEEPSEEK_API_KEY") -> str:
    """Resolve the DeepSeek key: env var first, then ~/.jetedge/secrets.env.
    Never prints or persists the key."""
    key = os.environ.get(env_key, "")
    if key:
        return key
    secrets = read_secrets_file(
        os.path.expanduser(cfg.get("secrets_file", "~/.jetedge/secrets.env")))
    return secrets.get("DEEPSEEK_API_KEY", "")
