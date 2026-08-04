"""Rollback to the baseline snapshot with read-back verification (Stage 12).

rollback_config(baseline_id) restores every stream field; the agent then
reads /streams back and compares priority + infer_interval against the
baseline values it captured when it took the snapshot.
"""

from __future__ import annotations

from agent.http_client import HttpTransport
from agent.schemas import ToolCall


class RollbackResult:
    def __init__(self, ok: bool, reason: str = "", snapshot_id: str = ""):
        self.ok = ok
        self.reason = reason
        self.snapshot_id = snapshot_id


def rollback_to(http: HttpTransport, snapshot_id: str, baseline_streams: dict) -> RollbackResult:
    """Rollback + read-back verify.  `baseline_streams` maps stream_id →
    {"priority": str, "infer_interval": int} captured when the snapshot was
    taken."""
    try:
        data = http.post("/config/rollback", {"snapshot_id": snapshot_id})
    except Exception as exc:  # network/envelope errors surface as-is
        return RollbackResult(False, f"rollback request failed: {exc}")
    restored_id = data.get("snapshot_id", snapshot_id)

    try:
        statuses = http.get("/streams").get("streams", [])
    except Exception as exc:
        return RollbackResult(False, f"read-back failed: {exc}", restored_id)

    mismatches = []
    for st in statuses:
        sid = st.get("stream_id")
        expected = baseline_streams.get(sid)
        if expected is None:
            continue
        if (st.get("priority") != expected["priority"]
                or st.get("infer_interval") != expected["infer_interval"]):
            mismatches.append(f"{sid}: priority={st.get('priority')} "
                              f"interval={st.get('infer_interval')} "
                              f"(expected {expected['priority']}/{expected['infer_interval']})")
    if mismatches:
        return RollbackResult(False, "read-back mismatch: " + "; ".join(mismatches),
                              restored_id)
    return RollbackResult(True, "", restored_id)


def capture_baseline_streams(observation: dict) -> dict:
    """Extract {stream_id: {"priority", "infer_interval"}} from a
    get_all_stream_status observation for read-back comparison."""
    out = {}
    for st in observation.get("streams", []):
        sid = st.get("stream_id")
        if sid:
            out[sid] = {
                "priority": st.get("priority"),
                "infer_interval": st.get("infer_interval"),
            }
    return out
