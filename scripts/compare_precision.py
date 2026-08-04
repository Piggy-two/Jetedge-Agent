#!/usr/bin/env python3
"""FP16 vs INT8 accuracy comparison for the Stage 13 regression.

Usage:
    python3 scripts/compare_precision.py \
        --fp16 logs/regression_fp16_detections.jsonl \
        --int8 logs/regression_int8_detections.jsonl \
        [--ort logs/ort_reference.jsonl] \
        [--report logs/stage13_precision_report.md]

Primary metric: FP16 vs INT8 through the SAME pipeline (only precision
differs — shared parser/NMS/preprocessing), matched per frame with
class-agnostic greedy IoU matching (IoU >= 0.5).  A class flip shows up as
unmatched + extra instead of being accepted.

Secondary: INT8 vs ORT FP32 within a "protocol band" of the FP16 vs ORT
baseline (numpy NMS vs nvinfer NMS implementation differences are a
systematic noise floor; INT8 must not fall more than 3 pp below it).

Prints a Markdown report and exits 0 when every threshold passes, 1
otherwise.  Thresholds below are conservative initial values; the final
values are set from the measured data.
"""

import argparse
import collections
import json
import math
import os
import sys

sys.path.insert(0, os.path.dirname(__file__))
from accuracy_math import (  # noqa: E402
    conf_deltas,
    greedy_match,
    match_rate,
    mean,
    pct,
    xywh_to_xyxy,
)

KEY_CLASSES = {"bus": 5, "car": 2, "person": 0}

# Conservative initial thresholds (final values from measured data).
THRESHOLDS = {
    "match_rate_fp16_to_int8": 0.95,
    "match_rate_int8_to_fp16": 0.95,
    "delta_conf_mean": 0.05,
    "delta_conf_p95": 0.10,
    "class_consistency": 0.98,
    "frame_count_delta_mean": 0.5,
    "key_target_retention": 0.95,
    "ort_protocol_band_pp": 3.0,
}


def load_jsonl(path: str) -> dict:
    """(stream_id, frame_num) -> list of dets in xyxy form."""
    per_frame: dict[tuple, list[dict]] = collections.defaultdict(list)
    with open(path, "r", encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            rec = json.loads(line)
            x1, y1, x2, y2 = xywh_to_xyxy(rec["bbox"])
            per_frame[(rec["stream_id"], rec["frame_num"])].append({
                "class_id": rec["class_id"],
                "confidence": float(rec["confidence"]),
                "x1": x1, "y1": y1, "x2": x2, "y2": y2,
            })
    return per_frame


def compare_frames(dets_a: dict, dets_b: dict, key: str) -> dict:
    """Per-frame greedy match, aggregated across all shared frames.

    Match indices are only meaningful within a frame, so every statistic is
    accumulated frame by frame."""
    n_a = n_b = n_matches = 0
    consist_same = consist_total = 0
    conf_diffs: list[float] = []
    count_deltas: list[float] = []
    shared = dets_a.keys() & dets_b.keys()
    for frame_key in sorted(shared):
        da = dets_a[frame_key]
        db = dets_b[frame_key]
        matches, _, _ = greedy_match(da, db)
        n_a += len(da)
        n_b += len(db)
        n_matches += len(matches)
        conf_diffs.extend(conf_deltas(matches, da, db))
        count_deltas.append(abs(len(da) - len(db)))
        for ia, ib, _ in matches:
            consist_same += int(da[ia]["class_id"] == db[ib]["class_id"])
            consist_total += 1
    return {
        "key": key,
        "shared_frames": len(shared),
        "frames_a": len(dets_a),
        "frames_b": len(dets_b),
        "dets_a": n_a,
        "dets_b": n_b,
        "matches": n_matches,
        "match_rate_a_to_b": match_rate([0] * n_matches, n_a),
        "match_rate_b_to_a": match_rate([0] * n_matches, n_b),
        "conf_delta_mean": mean(conf_diffs),
        "conf_delta_p95": pct(conf_diffs, 95),
        "conf_delta_max": max(conf_diffs) if conf_diffs else 0.0,
        "class_consistency": (consist_same / consist_total
                              if consist_total else 1.0),
        "count_delta_mean": mean(count_deltas),
    }


def key_target_retention(dets_fp16: dict, dets_int8: dict,
                         class_id: int) -> float:
    """Fraction of FP16 detections of `class_id` that still find an IoU
    match (class-agnostic) in INT8 on the same frame."""
    total = 0
    matched = 0
    for frame_key, da in dets_fp16.items():
        targets = [i for i, d in enumerate(da) if d["class_id"] == class_id]
        if not targets:
            continue
        db = dets_int8.get(frame_key, [])
        matches, _, _ = greedy_match(da, db)
        matched_ids = {m[0] for m in matches}
        total += len(targets)
        matched += sum(1 for i in targets if i in matched_ids)
    return matched / total if total else 1.0


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fp16", required=True)
    parser.add_argument("--int8", required=True)
    parser.add_argument("--ort", default=None)
    parser.add_argument("--report", default=None)
    args = parser.parse_args()

    fp16 = load_jsonl(args.fp16)
    int8 = load_jsonl(args.int8)
    ort = load_jsonl(args.ort) if args.ort else None

    # ---- Frame alignment (informational) ----------------------------------
    # JSONL holds one record per DETECTION, so frames with zero detections
    # leave no record: differing record counts only reflect detection
    # counts.  Real dropped frames are checked against the pipeline metrics
    # summary (in_frames per stream), verified manually before running.
    by_stream = collections.defaultdict(set)
    for s, f in fp16.keys() | int8.keys():
        by_stream[s].add(f)
    report: list[str] = [f"# Stage 13 INT8 accuracy regression report\n"]
    report.append("## Data\n")
    report.append(f"- fp16: `{args.fp16}` ({len(fp16)} frame-records)")
    report.append(f"- int8: `{args.int8}` ({len(int8)} frame-records)")
    if ort:
        report.append(f"- ort : `{args.ort}` ({len(ort)} frame-records)")
    report.append("")

    report.append("## Frame records (per detection; zero-detection frames "
                  "leave no record)\n")
    for sid in sorted(by_stream):
        f16_frames = len({f for s, f in fp16.keys() if s == sid})
        i8_frames = len({f for s, f in int8.keys() if s == sid})
        report.append(f"- {sid}: fp16 {f16_frames} recorded frames, "
                      f"int8 {i8_frames} recorded frames")
    report.append("")

    # ---- Primary: FP16 vs INT8 --------------------------------------------
    report.append("## FP16 vs INT8 (primary, same pipeline)\n")
    report.append("| stream | frames | f16 dets | i8 dets | match f16->i8 | "
                  "match i8->f16 | dconf mean | dconf p95 | dconf max | "
                  "class same | dn mean |")
    report.append("|---|---|---|---|---|---|---|---|---|---|---|")
    per_stream: dict[str, dict] = {}
    for sid in sorted({s for s, _ in fp16.keys()}):
        f16_s = {k: v for k, v in fp16.items() if k[0] == sid}
        i8_s = {k: v for k, v in int8.items() if k[0] == sid}
        r = compare_frames(f16_s, i8_s, sid)
        per_stream[sid] = r
        report.append(
            f"| {sid} | {r['shared_frames']} | {r['dets_a']} | {r['dets_b']} | "
            f"{r['match_rate_a_to_b']:.4f} | {r['match_rate_b_to_a']:.4f} | "
            f"{r['conf_delta_mean']:.4f} | {r['conf_delta_p95']:.4f} | "
            f"{r['conf_delta_max']:.4f} | {r['class_consistency']:.4f} | "
            f"{r['count_delta_mean']:.4f} |")
    report.append("")

    # ---- Key target retention ---------------------------------------------
    report.append("## Key target retention (FP16 detections kept by INT8)\n")
    report.append("| class | frames with target | retained | rate |")
    report.append("|---|---|---|---|")
    retention: dict[str, float] = {}
    for name, cls in KEY_CLASSES.items():
        total = sum(1 for s, d in fp16.items()
                    if any(x["class_id"] == cls for x in d))
        if total == 0:
            report.append(f"| {name} ({cls}) | 0 | - | n/a |")
            retention[name] = 1.0
            continue
        rate = key_target_retention(fp16, int8, cls)
        retention[name] = rate
        report.append(f"| {name} ({cls}) | {total} | {int(round(rate * total))} "
                      f"| {rate:.4f} |")
    report.append("")

    # ---- Secondary: vs ORT protocol band ----------------------------------
    ort_band = {}
    if ort:
        report.append("## vs ORT FP32 (protocol band)\n")
        report.append("| stream | fp16->ort rate | int8->ort rate | "
                      "band check (int8 >= fp16 - 3pp) |")
        report.append("|---|---|---|---|")
        for sid in sorted({s for s, _ in fp16.keys()}):
            f16_s = {k: v for k, v in fp16.items() if k[0] == sid}
            i8_s = {k: v for k, v in int8.items() if k[0] == sid}
            ort_s = {k: v for k, v in ort.items() if k[0] == sid}
            if not ort_s:
                continue
            r16 = compare_frames(f16_s, ort_s, sid)
            r18 = compare_frames(i8_s, ort_s, sid)
            band = r18["match_rate_a_to_b"] >= \
                r16["match_rate_a_to_b"] - THRESHOLDS["ort_protocol_band_pp"] / 100.0
            ort_band[sid] = band
            report.append(f"| {sid} | {r16['match_rate_a_to_b']:.4f} | "
                          f"{r18['match_rate_a_to_b']:.4f} | "
                          f"{'PASS' if band else 'FAIL'} |")
        report.append("")

    # ---- Threshold verdicts ------------------------------------------------
    report.append("## Threshold verdicts\n")
    report.append("| metric | threshold | actual | verdict |")
    report.append("|---|---|---|---|")
    checks = []
    for sid, r in per_stream.items():
        checks.append(("match_rate_fp16_to_int8", sid,
                       r["match_rate_a_to_b"]))
        checks.append(("match_rate_int8_to_fp16", sid,
                       r["match_rate_b_to_a"]))
        checks.append(("delta_conf_mean", sid, r["conf_delta_mean"]))
        checks.append(("delta_conf_p95", sid, r["conf_delta_p95"]))
        checks.append(("class_consistency", sid, r["class_consistency"]))
        checks.append(("frame_count_delta_mean", sid, r["count_delta_mean"]))
    for name, cls in KEY_CLASSES.items():
        checks.append(("key_target_retention", name, retention[name]))
    if ort:
        for sid, ok in ort_band.items():
            checks.append(("ort_protocol_band_pp", sid, 1.0 if ok else 0.0))

    all_pass = True
    for name, sid, actual in checks:
        th = THRESHOLDS[name]
        if name in ("delta_conf_mean", "delta_conf_p95",
                    "frame_count_delta_mean"):
            ok = actual <= th
        elif name == "ort_protocol_band_pp":
            ok = bool(actual)
        else:
            ok = actual >= th
        all_pass &= ok
        if name == "ort_protocol_band_pp":
            report.append(f"| {name} ({sid}) | >= fp16-3pp | "
                          f"{'PASS' if ok else 'FAIL'} | "
                          f"{'PASS' if ok else 'FAIL'} |")
        else:
            report.append(f"| {name} ({sid}) | {th:g} | {actual:.4f} | "
                          f"{'PASS' if ok else 'FAIL'} |")
    report.append("")
    report.append(f"**Overall: {'PASS' if all_pass else 'FAIL'}**\n")

    text = "\n".join(report)
    if args.report:
        with open(args.report, "w", encoding="utf-8") as fh:
            fh.write(text)
        print(f"report written to {args.report}")
    print(text)
    return 0 if all_pass else 1


if __name__ == "__main__":
    sys.exit(main())
