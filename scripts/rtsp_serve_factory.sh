#!/usr/bin/env bash
# rtsp_serve_factory.sh — publish the 4 factory-scenario videos to MediaMTX
# (rtsp://127.0.0.1:8554/cam1..cam4) for the intrusion scenario.
#
# Shares the same MediaMTX server + config as scripts/rtsp_serve.sh (paths
# cam1..cam4 already declared), but publishes DIFFERENT videos through its own
# pid directory (~/jetedge-rtsp/run-factory/) so it never collides with a
# Stage 14/15 demo publish (whose pids live in ~/jetedge-rtsp/run/).
#
# Videos: intel-iot-devkit sample-videos (CC-BY 4.0, ~/jetedge-openvideos/,
# outside Git). cam3 = one-by-one-person-detection (continuous person motion,
# long tracks) — the scenario's restricted_zone camera. cam4 = fault-injection
# target (people-detection crowd).
#
# Usage: same verbs as rtsp_serve.sh + `running-cams`.

set -u

REPO="$(cd "$(dirname "$0")/.." && pwd)"
MEDIAMTX_DIR="${MEDIAMTX_DIR:-$HOME/jetedge-rtsp}"
RUN_DIR="$MEDIAMTX_DIR/run-factory"
RTSP_URL="rtsp://127.0.0.1:8554"
OPENVIDEOS="$HOME/jetedge-openvideos"

declare -A VIDEO=(
  [cam1]="$OPENVIDEOS/person-bicycle-car-detection.mp4"   # 12 fps, entrance
  [cam2]="$OPENVIDEOS/car-detection.mp4"                  # 12.5 fps, parking
  [cam3]="$OPENVIDEOS/one-by-one-person-detection.mp4"    # 10 fps, long tracks
  [cam4]="$OPENVIDEOS/people-detection.mp4"               # 12 fps, corridor
)

log() { echo "[$(date '+%H:%M:%S')] $*"; }

server_start() { "$REPO/scripts/rtsp_serve.sh" server-start; }
server_stop()  { "$REPO/scripts/rtsp_serve.sh" server-stop; }

cam_start() {
  local cam="$1"
  local video="${VIDEO[$cam]:-}"
  if [ -z "$video" ] || [ ! -f "$video" ]; then
    log "ERROR: no scenario video for $cam (have: ${!VIDEO[*]})"
    return 1
  fi
  if [ -f "$RUN_DIR/$cam.pid" ] && kill -0 "$(cat "$RUN_DIR/$cam.pid")" 2>/dev/null; then
    log "$cam already publishing (pid $(cat "$RUN_DIR/$cam.pid"))"
    return 0
  fi
  mkdir -p "$RUN_DIR"
  # Same publish recipe as rtsp_serve.sh: MP4 AVCC -> Annex-B, TCP transport
  # (deterministic on loopback, verified at Stage 8).
  nohup ffmpeg -hide_banner -loglevel error -re -stream_loop -1 \
    -i "$video" -c copy -bsf:v h264_mp4toannexb \
    -rtsp_transport tcp -f rtsp "$RTSP_URL/$cam" \
    > "$RUN_DIR/$cam.log" 2>&1 &
  echo $! > "$RUN_DIR/$cam.pid"
  sleep 1
  if kill -0 "$(cat "$RUN_DIR/$cam.pid")" 2>/dev/null; then
    log "$cam publishing -> $RTSP_URL/$cam (pid $(cat "$RUN_DIR/$cam.pid"))"
  else
    log "ERROR: $cam publish process exited immediately (see $RUN_DIR/$cam.log)"
    rm -f "$RUN_DIR/$cam.pid"
    return 1
  fi
}

cam_stop() {
  local cam="$1"
  if [ -f "$RUN_DIR/$cam.pid" ]; then
    kill "$(cat "$RUN_DIR/$cam.pid")" 2>/dev/null
    rm -f "$RUN_DIR/$cam.pid"
    log "$cam publish stopped"
  else
    log "$cam not publishing"
  fi
}

# Is THIS cam being published by either run dir?
cam_active() {
  local cam="$1" pid
  for d in "$RUN_DIR" "$MEDIAMTX_DIR/run"; do
    [ -f "$d/$cam.pid" ] || continue
    pid=$(cat "$d/$cam.pid")
    if kill -0 "$pid" 2>/dev/null; then return 0; fi
  done
  return 1
}

running_cams() {
  for cam in cam1 cam2 cam3 cam4; do
    if cam_active "$cam"; then echo "$cam"; fi
  done
}

status() {
  log "mediamtx: $(server_state)"
  for cam in cam1 cam2 cam3 cam4; do
    if cam_active "$cam"; then log "$cam: publishing"; else log "$cam: stopped"; fi
  done
}

server_state() {
  if [ -f "$MEDIAMTX_DIR/run/mediamtx.pid" ] && \
     kill -0 "$(cat "$MEDIAMTX_DIR/run/mediamtx.pid")" 2>/dev/null; then
    echo "running (pid $(cat "$MEDIAMTX_DIR/run/mediamtx.pid"))"
  else
    echo "stopped"
  fi
}

case "${1:-}" in
  status)        status ;;
  server-start)  server_start ;;
  server-stop)   server_stop ;;
  cam-start)     [ $# -ge 2 ] && cam_start "$2" || { echo "usage: $0 cam-start <cam>"; exit 1; } ;;
  cam-stop)      [ $# -ge 2 ] && cam_stop "$2" || { echo "usage: $0 cam-stop <cam>"; exit 1; } ;;
  cam-restart)   [ $# -ge 2 ] && { cam_stop "$2"; sleep 1; cam_start "$2"; } \
                    || { echo "usage: $0 cam-restart <cam>"; exit 1; } ;;
  all-start)     server_start; sleep 1; for c in cam1 cam2 cam3 cam4; do cam_start "$c"; done ;;
  all-stop)      for c in cam4 cam3 cam2 cam1; do cam_stop "$c"; done ;;
  running-cams)  running_cams ;;
  *) echo "usage: $0 {status|server-start|server-stop|cam-start|cam-stop|cam-restart|all-start|all-stop|running-cams} [cam]"; exit 1 ;;
esac
