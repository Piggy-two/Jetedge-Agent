#!/usr/bin/env bash
# start_pipeline.sh — start jetedge_server and apply Stage 10 decoder CPU
# affinity (measured: eliminating decoder thread migration removed the
# wakeup->run tail latency, p99 45ms -> 1.5ms, zero end-to-end regression,
# see docs/stage10_ftrace.md).
#
# The 4 hardware-decode threads (src-camN-decode) and 4 DeepStream V4L2
# decode threads are GStreamer/DeepStream-internal threads, so affinity
# cannot be set from application code; the launch script pins them instead:
#   cam1 -> cpu0, cam2 -> cpu1, cam3 -> cpu2, cam4 -> cpu3
# (one decoder pair per core, the measured-good configuration).
#
# Usage:
#   start_pipeline.sh [config.yaml]     # default configs/streams_stage9.yaml
#   start_pipeline.sh stop              # SIGINT (graceful) and wait
#   start_pipeline.sh status            # running pid + affinity state
#
# No sudo, no system packages. Reversible: `stop` undoes nothing persistent
# (affinity dies with the process).

set -u

BIN="$(cd "$(dirname "$0")/.." && pwd)/build/jetedge_server"
REPO="$(cd "$(dirname "$0")/.." && pwd)"
CONFIG="configs/streams_stage9.yaml"
LOG="$REPO/logs/jetedge_server.log"

pin_decoder_threads() {  # <main pid>
  local pid="$1" cpu=0
  # src-camN-decode -> named core; V4L2_DecThread -> round-robin 0..3
  for tid in /proc/$pid/task/*; do
    tid="${tid##*/}"
    local comm
    comm=$(cat "/proc/$pid/task/$tid/comm" 2>/dev/null || true)
    case "$comm" in
      src-cam1-decode) taskset -pc 0 "$tid" >/dev/null 2>&1 ;;
      src-cam2-decode) taskset -pc 1 "$tid" >/dev/null 2>&1 ;;
      src-cam3-decode) taskset -pc 2 "$tid" >/dev/null 2>&1 ;;
      src-cam4-decode) taskset -pc 3 "$tid" >/dev/null 2>&1 ;;
      V4L2_DecThread)
        taskset -pc "$cpu" "$tid" >/dev/null 2>&1
        cpu=$(( (cpu + 1) % 4 ))
        ;;
    esac
  done
}

start() {
  if [ -f "$LOG" ]; then :; fi
  "$BIN" "$CONFIG" > "$LOG" 2>&1 &
  local pid=$!
  echo "[start_pipeline] jetedge_server started pid=$pid (config $CONFIG, log $LOG)"
  # RTSP streams take a few seconds to reach RUNNING and create decode threads.
  for s in 5 10 15 20 25 30; do
    sleep 5
    local found
    found=$(grep -lE 'src-cam[1-4]-decode' /proc/$pid/task/*/comm 2>/dev/null | wc -l)
    if [ "$found" -ge 4 ]; then
      pin_decoder_threads "$pid"
      echo "[start_pipeline] decode threads pinned (found $found)"
      return 0
    fi
  done
  echo "[start_pipeline] WARN: decode threads not fully detected after 30s (found ${found:-0}); skipping pin" >&2
  return 1
}

stop() {
  local pid
  pid=$(pgrep -x jetedge_server | head -1 || true)
  if [ -z "$pid" ]; then
    echo "[start_pipeline] no jetedge_server running"
    return 0
  fi
  echo "[start_pipeline] sending SIGINT to pid $pid"
  kill -INT "$pid"
  for _ in $(seq 1 30); do
    kill -0 "$pid" 2>/dev/null || { echo "[start_pipeline] stopped"; return 0; }
    sleep 1
  done
  echo "[start_pipeline] WARN: still running after 30s" >&2
  return 1
}

status() {
  local pid
  pid=$(pgrep -x jetedge_server | head -1 || true)
  if [ -z "$pid" ]; then
    echo "[start_pipeline] not running"
    return 0
  fi
  echo "[start_pipeline] running pid=$pid"
  for tid in /proc/$pid/task/*; do
    tid="${tid##*/}"
    local comm
    comm=$(cat "/proc/$pid/task/$tid/comm" 2>/dev/null || true)
    case "$comm" in
      src-cam*-decode|V4L2_DecThread)
        echo "  $tid $comm -> $(taskset -pc "$tid" 2>/dev/null | grep -oE '[0-9,-]+$')"
        ;;
    esac
  done
}

# Only run the CLI when invoked directly (not sourced).
if [ "${BASH_SOURCE[0]}" = "$0" ]; then
  case "${1:-}" in
    start)  shift; start ;;
    stop)   stop ;;
    status) status ;;
    *) CONFIG="${1:-configs/streams_stage9.yaml}"; start ;;
  esac
fi
