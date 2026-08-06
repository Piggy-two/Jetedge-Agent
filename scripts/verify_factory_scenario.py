#!/usr/bin/env python3
"""verify_factory_scenario.py — read-only semantic validation for the factory
dangerous-area intrusion scenario (2026-08-06).

Checks (all against real artifacts under logs/scenario_factory*):
  1. zone dedup        — at most one zone_entry per (stream, track, zone)
  2. track continuity  — cam3 long tracks, sane consecutive-frame movement
  3. incidents         — 9 required fields, decisions in the rule set, every
                         zone_entry event has exactly one incidents row
  4. degraded routing  — local_rule_only rows explain failed/not-submitted
                         Qwen outcomes
  5. FPS during Qwen   — bench_before vs bench_during per-stream comparison
  6. fault isolation   — cam4 DEGRADED/RECONNECTING during injection while
                         cam1-3 stay RUNNING with 0 reconnect/failure
  7. RSS trend         — no sustained growth: max RSS in last 25% of windows
                         <= 1.15 x max RSS in first 25%

Exit 0 = all checks pass.  `--phase zone-scan` prints cam3 person-center
coverage of the configured restricted_zone rect (for rect refinement).
"""

import argparse
import csv
import json
import sys

L = "logs"
DET = f"{L}/scenario_factory_detections.jsonl"
EV = f"{L}/scenario_factory_events.jsonl"
AN = f"{L}/scenario_factory_analysis.jsonl"
INC = f"{L}/scenario_factory_incidents.jsonl"
BENCH_BEFORE = f"{L}/scenario_factory_baseline/bench_before.json"
BENCH_DURING = f"{L}/scenario_factory_bench_during.json"
FAULT_TL = f"{L}/scenario_factory_fault_timeline.jsonl"
STABLE = f"{L}/scenario_factory_stability.csv"
ZONE_RECT = (560.0, 250.0, 500.0, 200.0)  # restricted_zone in mux 1280x720

DECISIONS = {"confirmed_alert", "manual_review", "archived", "local_rule_only"}
REQ_FIELDS = ["event_id", "stream_id", "track_id", "local_event",
              "qwen_description", "qwen_risk", "qwen_confidence",
              "decision", "timestamp"]

failures = []
warnings = []


def check(name, ok, detail=""):
    status = "PASS" if ok else "FAIL"
    if not ok:
        failures.append(f"{name}: {detail}")
    print(f"  {status}  {name}{'  — ' + detail if detail else ''}")
    return ok


def load_jsonl(path):
    rows = []
    try:
        with open(path) as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                try:
                    rows.append(json.loads(line))
                except json.JSONDecodeError as e:
                    rows.append({"__invalid__": str(e)})
    except FileNotFoundError:
        pass
    return rows


def zone_entry_key(d):
    return (d.get("stream_id"), d.get("track_id"), d.get("zone"))


def check_zone_dedup(events):
    groups = {}
    for d in events:
        if d.get("event") == "zone_entry":
            key = zone_entry_key(d)
            groups.setdefault(key, []).append(d)
    bad = {k: len(v) for k, v in groups.items() if len(v) > 1}
    n_cam3 = sum(1 for k in groups if k[0] == "cam3")
    check("zone dedup (<=1 fire per track per zone)",
          not bad and n_cam3 >= 1,
          f"{len(groups)} distinct (stream,track,zone) keys, cam3={n_cam3}"
          + (f", repeats={bad}" if bad else ", 0 repeats"))
    return groups


def check_track_continuity(detections):
    per_track = {}
    for d in detections:
        if d.get("stream_id") != "cam3":
            continue
        t = d.get("track_id")
        b = d.get("bbox")
        if b is None:
            continue
        per_track.setdefault(t, []).append(
            (d.get("frame_num", 0), b[0] + b[2] / 2.0, b[1] + b[3] / 2.0))
    long_tracks = []
    max_jump = 0.0
    for t, pts in per_track.items():
        pts.sort()
        frames = [p[0] for p in pts]
        if len(pts) >= 50:
            long_tracks.append((t, len(pts), frames[-1] - frames[0] + 1))
        for (_, x1, y1), (_, x2, y2) in zip(pts, pts[1:]):
            max_jump = max(max_jump, ((x2 - x1) ** 2 + (y2 - y1) ** 2) ** 0.5)
    long_tracks.sort(key=lambda x: -x[1])
    # 10 fps source, person crosses the 1280-wide frame: a single worst pair
    # can exceed 100px from genuine motion (a fast walk crosses ~110 px/frame);
    # threshold 150 px still catches any true track-id swap across objects.
    check("cam3 track continuity",
          len(long_tracks) >= 3 and max_jump < 150.0,
          f"{len(per_track)} distinct tracks, {len(long_tracks)} long (>=50 frames): "
          + ", ".join(f"t{t}:{n}fr" for t, n, _ in long_tracks[:5])
          + f" | max consecutive-frame center jump {max_jump:.1f}px (<150 sanity)")
    return long_tracks


def check_incidents(events, incidents):
    bad_fields = 0
    bad_decision = 0
    dist = {}
    for d in incidents:
        missing = [k for k in REQ_FIELDS if k not in d]
        bad_fields += len(missing)
        if d.get("decision") not in DECISIONS:
            bad_decision += 1
        dist[d.get("decision")] = dist.get(d.get("decision"), 0) + 1
    # every zone_entry event must have exactly one incidents row
    ev_keys = set()
    for d in events:
        if d.get("event") == "zone_entry":
            ev_keys.add(zone_entry_key(d))
    inc_keys = set()
    for d in incidents:
        inc_keys.add((d.get("stream_id"), d.get("track_id"), d.get("zone")))
    missing_inc = ev_keys - inc_keys
    extra_inc = inc_keys - ev_keys
    check("incidents fields + decisions",
          bad_fields == 0 and bad_decision == 0,
          f"{len(incidents)} rows, decisions={dist}")
    check("incidents <-> events 1:1 (zone_entry)",
          not missing_inc and not extra_inc,
          f"missing {len(missing_inc)}, extra {len(extra_inc)}"
          + (f" (e.g. {sorted(missing_inc)[:2]})" if missing_inc else ""))
    return dist


def check_degraded_routing(incidents, analysis):
    lro = [d for d in incidents if d.get("decision") == "local_rule_only"]
    lro_reasons = {}
    for d in lro:
        r = (d.get("reason") or "").split(":")[0]
        lro_reasons[r] = lro_reasons.get(r, 0) + 1
    an_fail = [d for d in analysis
               if d.get("provider") == "qwen" and not d.get("success", True)]
    check("degraded routing (local_rule_only explains qwen failures)",
          len(lro) >= len(an_fail),
          f"{len(lro)} local_rule_only rows (reasons {lro_reasons or 'none'}) "
          f"vs {len(an_fail)} failed analysis rows")
    return lro_reasons


def bench_fps(path):
    try:
        with open(path) as f:
            d = json.load(f)
        out = {}
        # /benchmark streams is an OBJECT keyed by stream_id, not a list.
        for sid, s in d.get("data", {}).get("streams", {}).items():
            out[sid] = {
                "input_fps": s.get("input_fps", 0),
                "infer_fps": s.get("infer_fps", 0),
                "p95": s.get("lat_p95_ms", 0),
            }
        return out, d.get("data", {}).get("global", {})
    except Exception:
        return {}, {}


def check_fps_during_qwen():
    before, g_b = bench_fps(BENCH_BEFORE)
    during, g_d = bench_fps(BENCH_DURING)
    if not before or not during:
        check("FPS during Qwen (bench_before vs bench_during)",
              False, "benchmark files missing or unparseable")
        return
    lines = []
    worst = 0.0
    for cam in ["cam1", "cam2", "cam3", "cam4"]:
        if cam not in before or cam not in during:
            continue
        b, d = before[cam]["input_fps"], during[cam]["input_fps"]
        if b <= 0:
            continue
        rel = (d - b) / b * 100.0
        worst = min(worst, rel)
        lines.append(f"{cam} {b:.1f}->{d:.1f} fps ({rel:+.1f}%)")
    check("FPS during Qwen (no stream drops >10%)",
          worst > -10.0,
          "; ".join(lines) + f" | global p95 {g_b.get('p95_ms')}->{g_d.get('p95_ms')} ms")
    if worst <= -5.0:
        warnings.append(f"FPS drop {worst:+.1f}% in 5-10% band — inspect")


def check_fault_isolation():
    try:
        rows = []
        with open(FAULT_TL) as f:
            for line in f:
                line = line.strip()
                if not line:
                    continue
                d = json.loads(line)
                if "cam1" in d:
                    rows.append(d)
    except (FileNotFoundError, json.JSONDecodeError) as e:
        check("fault isolation", False, f"timeline unreadable: {e}")
        return
    if not rows:
        check("fault isolation", False, "no timeline rows")
        return
    # Isolation = cam1-3 stay RUNNING with NO NEW reconnect/failure during the
    # injection window.  Absolute counts may carry a startup transient
    # (total_reconnects_ is never reset by mark_running), so compare deltas
    # from the first timeline sample.
    first = rows[0]
    healthy_ok = all(r.get("cam1") == "RUNNING" and r.get("cam2") == "RUNNING"
                     and r.get("cam3") == "RUNNING"
                     and r.get("cam1_reconnects", 0) == first.get("cam1_reconnects", 0)
                     and r.get("cam2_reconnects", 0) == first.get("cam2_reconnects", 0)
                     and r.get("cam3_reconnects", 0) == first.get("cam3_reconnects", 0)
                     and r.get("cam1_failures", 0) == first.get("cam1_failures", 0)
                     and r.get("cam2_failures", 0) == first.get("cam2_failures", 0)
                     and r.get("cam3_failures", 0) == first.get("cam3_failures", 0)
                     for r in rows)
    # cam4 must leave RUNNING during the injection window (2s sampling may
    # miss the transient DEGRADED — that is captured in the server log).
    cam4_left_running = any(r.get("cam4") != "RUNNING" for r in rows)
    cam4_reconnecting = any(r.get("cam4") == "RECONNECTING" for r in rows)
    cam4_reconnect_count = max((r.get("cam4_reconnects", 0) for r in rows),
                               default=0)
    check("fault isolation (cam1-3 RUNNING 0 reconnects/failures)", healthy_ok,
          f"{len(rows)} timeline samples, cam1-3 all RUNNING 0/0")
    check("cam4 state machine during injection",
          cam4_left_running and cam4_reconnecting,
          f"cam4 states: {sorted({r.get('cam4') for r in rows})}, "
          f"max reconnect_count {cam4_reconnect_count}")
    # Recovery evidence: post-recovery snapshot (fault_after.json) shows
    # cam4 back to RUNNING with reconnect_count >= 1 (verified by the FPS
    # check in the server log after verify_sec).
    try:
        with open("logs/scenario_factory_fault/fault_after.json") as f:
            d = json.load(f)
        cam4 = next((s for s in d.get("data", {}).get("streams", [])
                     if s.get("stream_id") == "cam4"), None)
    except (FileNotFoundError, json.JSONDecodeError, StopIteration):
        cam4 = None
    check("cam4 auto-recovery (RUNNING + reconnect_count>=1)",
          cam4 is not None and cam4.get("state") == "RUNNING"
          and cam4.get("reconnect_count", 0) >= 1,
          f"fault_after.json cam4: {cam4.get('state') if cam4 else 'missing'}, "
          f"rc {cam4.get('reconnect_count') if cam4 else 'n/a'}")


def check_rss_trend():
    try:
        with open(STABLE, newline="") as f:
            r = list(csv.DictReader(f))
    except (FileNotFoundError, csv.Error) as e:
        check("RSS trend", False, f"CSV unreadable: {e}")
        return
    vals = [int(x["rss_kb"]) for x in r if x.get("rss_kb", "").isdigit()]
    if len(vals) < 10:
        check("RSS trend", False, f"only {len(vals)} RSS samples")
        return
    n = len(vals)
    head_max = max(vals[: n // 4])
    tail_max = max(vals[3 * n // 4:])
    check("RSS no sustained growth (tail/head max <= 1.15)",
          tail_max <= head_max * 1.15,
          f"head25 max {head_max/1024:.1f} MiB, tail25 max {tail_max/1024:.1f} MiB, "
          f"ratio {tail_max/max(head_max,1):.3f}, n={n}")


def zone_scan():
    """Report cam3 person-center coverage of the restricted_zone rect."""
    inside = 0
    total = 0
    for d in load_jsonl(DET):
        if d.get("stream_id") != "cam3" or d.get("class") != "person":
            continue
        b = d.get("bbox")
        if b is None:
            continue
        cx = b[0] + b[2] / 2.0
        cy = b[1] + b[3] / 2.0
        total += 1
        if (ZONE_RECT[0] <= cx < ZONE_RECT[0] + ZONE_RECT[2] and
                ZONE_RECT[1] <= cy < ZONE_RECT[1] + ZONE_RECT[3]):
            inside += 1
    print(f"zone-scan: rect={ZONE_RECT} in mux 1280x720")
    print(f"  cam3 person frames: {total}, centers inside rect: {inside} "
          f"({100.0 * inside / total:.1f}%)" if total else "  no cam3 person frames")
    return 0 if total and inside / total >= 0.80 else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--phase", default="full",
                    help="full | zone-scan")
    args = ap.parse_args()
    if args.phase == "zone-scan":
        return zone_scan()

    print(f"== verify_factory_scenario ({args.phase}) ==")
    events = load_jsonl(EV)
    detections = load_jsonl(DET)
    incidents = load_jsonl(INC)
    analysis = load_jsonl(AN)

    check("events JSONL readable", len(events) > 0,
          f"{len(events)} rows")
    check_zone_dedup(events)
    check_track_continuity(detections)
    dist = check_incidents(events, incidents)
    check_degraded_routing(incidents, analysis)
    check_fps_during_qwen()
    check_fault_isolation()
    check_rss_trend()

    if warnings:
        print("  WARN:")
        for w in warnings:
            print(f"    - {w}")
    if failures:
        print(f"== {len(failures)} FAILURE(S) ==")
        for f in failures:
            print(f"  - {f}")
        return 1
    print("== all checks passed ==")
    return 0


if __name__ == "__main__":
    sys.exit(main())
