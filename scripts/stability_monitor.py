#!/usr/bin/env python3
"""stability_monitor.py — sample jetedge_server health during the Stage 14
2-hour stability run.

Samples every `interval` seconds and appends one CSV row per sample:
  ts, t_s, rss_kb, cpu_pct, temp_c, load1, sched_state, sched_temp_c,
  <per stream> state, fps, p50, p95, p99, reconnects, failures, frames

System sources mirror the server's own samplers:
  RSS        /proc/<pid>/status VmRSS
  CPU        /proc/<pid>/stat utime+stime delta vs /proc/stat total delta
  temperature max /sys/class/thermal/thermal_zone*/temp (same as scheduler)
  load       /proc/loadavg
API sources (127.0.0.1:8090):
  /scheduler/state  state, temp_c
  /metrics/summary  per-stream lat_p50/p95/p99_ms, latest_input_fps, frames
  /streams          per-stream state, reconnect_count, consecutive_failures

Usage:
  stability_monitor.py --pid <pid> [--csv out.csv] [--interval 60]
                        [--max-duration 7800] [--api http://127.0.0.1:8090]
Exits 0 when the pid disappears or max duration is reached.
"""

import argparse
import csv
import json
import os
import sys
import time
import urllib.request


def read_rss_kb(pid):
    with open(f"/proc/{pid}/status") as f:
        for line in f:
            if line.startswith("VmRSS:"):
                return int(line.split()[1])
    return -1


def read_cpu_ticks(pid):
    """Return (proc_ticks, total_ticks) for a process and the whole system."""
    with open(f"/proc/{pid}/stat") as f:
        parts = f.read().split()
    # utime=14, stime=15 (1-indexed fields -> indexes 13, 14)
    proc = int(parts[13]) + int(parts[14])
    with open("/proc/stat") as f:
        total = sum(int(v) for v in f.readline().split()[1:])
    return proc, total


def read_max_temp_c():
    best = -1.0
    i = 0
    while True:
        path = f"/sys/class/thermal/thermal_zone{i}/temp"
        if not os.path.exists(path):
            break
        # Some zones (e.g. cv0-cv2 on Orin Nano) fail to read via buffered
        # file objects; use low-level os.open and ignore any failing zone.
        try:
            fd = os.open(path, os.O_RDONLY)
            try:
                raw = os.read(fd, 32)
            finally:
                os.close(fd)
            if raw and raw.strip():
                best = max(best, int(raw.strip()) / 1000.0)
        except (OSError, ValueError, TypeError):
            pass
        i += 1
    return best


def read_load1():
    with open("/proc/loadavg") as f:
        return float(f.read().split()[0])


def api_get(base, path):
    try:
        with urllib.request.urlopen(f"{base}{path}", timeout=5) as resp:
            return json.loads(resp.read().decode())
    except Exception:
        return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--pid", type=int, required=True)
    ap.add_argument("--csv", default="logs/stage14_stability_samples.csv")
    ap.add_argument("--interval", type=float, default=60.0)
    ap.add_argument("--max-duration", type=float, default=7800.0)
    ap.add_argument("--api", default="http://127.0.0.1:8090")
    args = ap.parse_args()

    os.makedirs(os.path.dirname(os.path.abspath(args.csv)), exist_ok=True)
    header = ["ts", "t_s", "rss_kb", "cpu_pct", "temp_c", "load1",
              "sched_state", "sched_temp_c"]
    streams = ["cam1", "cam2", "cam3", "cam4"]
    for s in streams:
        header += [f"{s}_state", f"{s}_fps", f"{s}_p50", f"{s}_p95",
                   f"{s}_p99", f"{s}_reconnects", f"{s}_failures", f"{s}_frames"]
    t0 = time.monotonic()
    prev_proc = prev_total = None
    cpu_pct = 0.0
    samples = 0
    with open(args.csv, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(header)
        while True:
            alive = os.path.exists(f"/proc/{args.pid}") and \
                os.path.exists(f"/proc/{args.pid}/status")
            elapsed = time.monotonic() - t0
            if not alive:
                print(f"[monitor] server pid {args.pid} exited after "
                      f"{elapsed:.0f}s; finalizing")
                break
            if elapsed > args.max_duration:
                print(f"[monitor] max duration {args.max_duration:.0f}s reached")
                break
            now = time.strftime("%Y-%m-%d %H:%M:%S")
            try:
                rss = read_rss_kb(args.pid)
            except FileNotFoundError:
                rss = -1
            proc, total = read_cpu_ticks(args.pid)
            if prev_proc is not None and total > prev_total:
                cpu_pct = (proc - prev_proc) / (total - prev_total) * 100.0
            prev_proc, prev_total = proc, total
            row = [now, f"{elapsed:.0f}", rss, f"{cpu_pct:.1f}",
                   f"{read_max_temp_c():.1f}", f"{read_load1():.2f}"]
            sched = api_get(args.api, "/scheduler/state")
            row.append((sched or {}).get("data", {}).get("state", "NA"))
            row.append((sched or {}).get("data", {}).get("temp_c", "NA"))
            summary = api_get(args.api, "/metrics/summary")
            sts = api_get(args.api, "/streams")
            by_id = {}
            if summary:
                for s in summary.get("data", {}).get("streams", []):
                    by_id[s["stream_id"]] = s
            status = {}
            if sts:
                for s in sts.get("data", {}).get("streams", []):
                    status[s["stream_id"]] = s
            for sid in streams:
                m = by_id.get(sid, {})
                st = status.get(sid, {})
                row += [
                    st.get("state", "NA"),
                    f"{m.get('latest_input_fps', -1):.1f}" if isinstance(
                        m.get("latest_input_fps"), (int, float)) else "NA",
                    f"{m.get('lat_p50_ms', -1):.1f}" if isinstance(
                        m.get("lat_p50_ms"), (int, float)) else "NA",
                    f"{m.get('lat_p95_ms', -1):.1f}" if isinstance(
                        m.get("lat_p95_ms"), (int, float)) else "NA",
                    f"{m.get('lat_p99_ms', -1):.1f}" if isinstance(
                        m.get("lat_p99_ms"), (int, float)) else "NA",
                    st.get("reconnect_count", "NA"),
                    st.get("consecutive_failures", "NA"),
                    st.get("frames", "NA"),
                ]
            w.writerow(row)
            f.flush()
            samples += 1
            print(f"[monitor] t={elapsed:.0f}s rss={rss}kB cpu={cpu_pct:.1f}% "
                  f"temp={row[4]}C sched={row[6]}", flush=True)
            time.sleep(args.interval)
    print(f"[monitor] done: {samples} samples written to {args.csv}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
