#!/usr/bin/env python3
"""Analyze the Stage 5 detection JSONL output.

Usage: python3 scripts/analyze_stage5_jsonl.py <detections.jsonl>

Prints per-stream frame counts, detection counts, class distribution,
track-id stability, and top detections by confidence.
"""

import collections
import json
import sys


def main() -> None:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} <detections.jsonl>")
        return 1

    path = sys.argv[1]
    per_stream: dict[str, list[dict]] = collections.defaultdict(list)
    with open(path, "r", encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            rec = json.loads(line)
            per_stream[rec["stream_id"]].append(rec)

    if not per_stream:
        print("no detections found")
        return 1

    print(f"{'stream':<10} {'frames':>8} {'detections':>10} {'obj/frame':>9} "
          f"{'tracks':>7}")
    total_frames = 0
    total_det = 0
    for sid, recs in sorted(per_stream.items()):
        frames = len({r["frame_num"] for r in recs})
        tracks = len({r["track_id"] for r in recs})
        total_frames += frames
        total_det += len(recs)
        print(f"{sid:<10} {frames:>8} {len(recs):>10} "
              f"{len(recs) / max(frames, 1):>9.2f} {tracks:>7}")

    print(f"\ntotal frames {total_frames}, total detections {total_det}")

    print("\nclass distribution:")
    classes = collections.Counter(r["class"] for recs in per_stream.values() for r in recs)
    for name, count in classes.most_common(10):
        print(f"  {name:<16} {count:>8}")

    print("\ntrack-id stability (top tracks by detection count):")
    for sid, recs in sorted(per_stream.items()):
        by_track = collections.defaultdict(list)
        for r in recs:
            by_track[r["track_id"]].append(r["frame_num"])
        top = sorted(by_track.items(), key=lambda kv: len(kv[1]), reverse=True)[:3]
        for tid, frames in top:
            if len(frames) < 2:
                continue
            span = max(frames) - min(frames) + 1
            gaps = sum(1 for a, b in zip(frames, frames[1:]) if b - a > 1)
            print(f"  {sid:<10} track={tid:<6} detections={len(frames):>4} "
                  f"frame_span={span:>4} gaps={gaps}")

    print("\ntop-5 detections by confidence:")
    all_recs = [r for recs in per_stream.values() for r in recs]
    for r in sorted(all_recs, key=lambda x: x["confidence"], reverse=True)[:5]:
        print(f"  {r['stream_id']} frame={r['frame_num']} track={r['track_id']} "
              f"class={r['class']} conf={r['confidence']:.3f} bbox={r['bbox']}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
