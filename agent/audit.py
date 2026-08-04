"""Agent-side audit trail and report writer (Stage 12).

Two outputs:
  * logs/agent/audit.jsonl   — one JSON line per phase, append-only
  * logs/agent/reports/<run_id>.md — human-readable run report; numbers are
    copied verbatim from tool results (never estimated).

The server keeps its own audit chain (logs/control/audit.jsonl); the agent
chain references server request/snapshot ids so the two can be correlated.
"""

from __future__ import annotations

import json
import os
import time


class AgentAudit:
    def __init__(self, path: str, run_id: str):
        self.path = path
        self.run_id = run_id
        self._fh = None
        if path:
            os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
            self._fh = open(path, "a", encoding="utf-8")

    def log(self, phase: str, ok: bool, **fields):
        if not self._fh:
            return
        record = {
            "ts_ms": int(time.time() * 1000),
            "run_id": self.run_id,
            "phase": phase,
            "ok": bool(ok),
        }
        record.update(fields)
        self._fh.write(json.dumps(record, ensure_ascii=False) + "\n")
        self._fh.flush()

    def close(self):
        if self._fh:
            self._fh.close()
            self._fh = None


def write_report(path: str, run_id: str, lines: list):
    """Write a markdown report (append-only run history per run_id file)."""
    os.makedirs(os.path.dirname(path) or ".", exist_ok=True)
    with open(path, "a", encoding="utf-8") as fh:
        fh.write(f"\n## Run {run_id}\n\n")
        for line in lines:
            fh.write(line + "\n")
