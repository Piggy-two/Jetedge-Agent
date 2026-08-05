#!/usr/bin/env python3
"""analyze_stability.py — summarize a stability_monitor.py CSV into the
Stage 14 acceptance table: RSS curve, latency percentiles, per-stream FPS,
reconnect counters, scheduler states, temperature.

Usage: analyze_stability.py <samples.csv>
"""

import csv
import sys


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else \
        "logs/stage14_stability_samples.csv"
    rows = list(csv.DictReader(open(path)))
    if not rows:
        print("no samples")
        return 1
    n = len(rows)
    streams = ["cam1", "cam2", "cam3", "cam4"]

    rss = [int(r["rss_kb"]) for r in rows]
    temp = [float(r["temp_c"]) for r in rows if r["temp_c"] != "NA"]
    cpu = [float(r["cpu_pct"]) for r in rows if r["cpu_pct"] != "NA"]
    states = {}
    for r in rows:
        states[r["sched_state"]] = states.get(r["sched_state"], 0) + 1
    runtime = float(rows[-1]["t_s"]) - float(rows[0]["t_s"])

    print(f"samples        : {n} (t={rows[0]['t_s']}s .. {rows[-1]['t_s']}s, "
          f"window {runtime:.0f}s)")
    print(f"RSS            : start {rss[0]/1024:.1f} MiB, end {rss[-1]/1024:.1f} "
          f"MiB, peak {max(rss)/1024:.1f} MiB, growth "
          f"{(rss[-1]-rss[0])/rss[0]*100:+.2f}%")
    print(f"temp           : mean {sum(temp)/len(temp):.1f} C, "
          f"max {max(temp):.1f} C")
    print(f"cpu (1 core)   : mean {sum(cpu)/len(cpu):.1f}%, max {max(cpu):.1f}%")
    print(f"sched states   : {states}")

    for s in streams:
        fps = [float(r[f"{s}_fps"]) for r in rows
               if r[f"{s}_fps"] not in ("NA", "")]
        p50 = [float(r[f"{s}_p50"]) for r in rows
               if r[f"{s}_p50"] not in ("NA", "")]
        p95 = [float(r[f"{s}_p95"]) for r in rows
               if r[f"{s}_p95"] not in ("NA", "")]
        p99 = [float(r[f"{s}_p99"]) for r in rows
               if r[f"{s}_p99"] not in ("NA", "")]
        rc = max(int(r[f"{s}_reconnects"]) for r in rows)
        fl = max(int(r[f"{s}_failures"]) for r in rows)
        states_s = set(r[f"{s}_state"] for r in rows)
        if not p95:
            print(f"{s}: no latency data")
            continue
        # first vs last 10% window for drift
        k = max(1, n // 10)
        dr = sum(p95[:k]) / len(p95[:k]) - sum(p95[-k:]) / len(p95[-k:])
        print(f"{s}: fps min/mean/max {min(fps):.1f}/{sum(fps)/len(fps):.1f}/"
              f"{max(fps):.1f} | P50 {sum(p50)/len(p50):.1f} "
              f"(min {min(p50):.1f}/max {max(p50):.1f}) | "
              f"P95 {sum(p95)/len(p95):.1f} (min {min(p95):.1f}/max "
              f"{max(p95):.1f}) | P99 {sum(p99)/len(p99):.1f} | "
              f"P95 drift first-vs-last 10% {dr:+.1f}ms | "
              f"reconnects {rc} failures {fl} states={sorted(states_s)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
