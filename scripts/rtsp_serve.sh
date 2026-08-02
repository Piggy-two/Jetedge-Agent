#!/usr/bin/env bash
# rtsp_serve.sh — start/stop the local MediaMTX RTSP server and per-camera
# ffmpeg publish loops for Stage 8 RTSP fault-isolation tests.
#
# All artifacts live OUTSIDE the Git repo (~/jetedge-rtsp/): the mediamtx
# binary, its config, and pid files. No sudo, no system packages.
#
# Usage:
#   rtsp_serve.sh status
#   rtsp_serve.sh server-start | server-stop | server-restart
#   rtsp_serve.sh cam-start  <cam1|cam2|cam3|cam4>
#   rtsp_serve.sh cam-stop   <cam>
#   rtsp_serve.sh cam-restart <cam>
#
# Cameras are published as rtsp://127.0.0.1:8554/<cam> (H.264, -c copy).

set -u

MEDIAMTX_DIR="${MEDIAMTX_DIR:-$HOME/jetedge-rtsp}"
MEDIAMTX_BIN="$MEDIAMTX_DIR/mediamtx"
MEDIAMTX_CONF="$MEDIAMTX_DIR/mediamtx.yml"
RUN_DIR="$MEDIAMTX_DIR/run"
STREAMS=/opt/nvidia/deepstream/deepstream-7.1/samples/streams
RTSP_URL="rtsp://127.0.0.1:8554"

# cam -> sample video mapping (same scenes as Stage 5/6/7).
# cam1 uses the mp4 container: this ffmpeg build cannot `-stream_loop` raw
# .h264 input (EPERM "Operation not permitted" on the loop-boundary seek —
# reproduced on a plain copy in /tmp, so it is an ffmpeg demuxer quirk, not
# the filesystem).  MP4/MOV inputs loop cleanly (verified 65s+).
# cam3/cam4 use PRE-REMUXED mp4 copies of the .mov samples: ffmpeg's mov
# demuxer + `-c copy` RTSP publish produces corrupted RTP even with
# h264_mp4toannexb (client: 88/53 decode errors per 25s, verified 2026-08-01);
# the same publish from an mp4 container is clean (0 errors / 25s, frames
# flow at source rate).  Remux was one-time: ffmpeg -i x.mov -c copy x_remux.mp4
# (outputs live in ~/jetedge-rtsp/, outside Git).
declare -A VIDEO=(
  [cam1]="$STREAMS/sample_720p.mp4"
  [cam2]="$STREAMS/sample_office.mp4"
  [cam3]="$MEDIAMTX_DIR/sample_walk_remux.mp4"
  [cam4]="$MEDIAMTX_DIR/sample_ride_bike_remux.mp4"
)

log() { echo "[$(date '+%H:%M:%S')] $*"; }

server_start() {
  if [ -f "$RUN_DIR/mediamtx.pid" ] && kill -0 "$(cat "$RUN_DIR/mediamtx.pid")" 2>/dev/null; then
    log "mediamtx already running (pid $(cat "$RUN_DIR/mediamtx.pid"))"
    return 0
  fi
  mkdir -p "$RUN_DIR"
  nohup "$MEDIAMTX_BIN" "$MEDIAMTX_CONF" > "$RUN_DIR/mediamtx.log" 2>&1 &
  echo $! > "$RUN_DIR/mediamtx.pid"
  sleep 1
  log "mediamtx started (pid $(cat "$RUN_DIR/mediamtx.pid"))"
}

server_stop() {
  if [ -f "$RUN_DIR/mediamtx.pid" ]; then
    kill "$(cat "$RUN_DIR/mediamtx.pid")" 2>/dev/null
    rm -f "$RUN_DIR/mediamtx.pid"
    log "mediamtx stopped"
  fi
}

cam_start() {
  local cam="$1"
  local video="${VIDEO[$cam]:-}"
  if [ -z "$video" ] || [ ! -f "$video" ]; then
    log "ERROR: no sample video for $cam (have: ${!VIDEO[*]})"
    return 1
  fi
  if [ -f "$RUN_DIR/$cam.pid" ] && kill -0 "$(cat "$RUN_DIR/$cam.pid")" 2>/dev/null; then
    log "$cam already publishing (pid $(cat "$RUN_DIR/$cam.pid"))"
    return 0
  fi
  # h264_mp4toannexb: MP4/MOV inputs carry H264 in AVCC (length-prefixed)
  # format; RTP requires Annex-B.  Without this bitstream filter the published
  # stream is corrupted ("Invalid level prefix" on any client, FU-A errors on
  # the server).  No-op for Annex-B inputs (raw .h264).
  # rtsp_transport tcp: four 1080p publishers over UDP dropped packets in the
  # server's receive socket (FU-A fragments lost mid-nal); TCP publish is
  # deterministic on loopback.
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

status() {
  log "mediamtx: $(server_state)"
  for cam in cam1 cam2 cam3 cam4; do
    if [ -f "$RUN_DIR/$cam.pid" ] && kill -0 "$(cat "$RUN_DIR/$cam.pid")" 2>/dev/null; then
      log "$cam: publishing (pid $(cat "$RUN_DIR/$cam.pid"))"
    else
      log "$cam: stopped"
    fi
  done
}

server_state() {
  if [ -f "$RUN_DIR/mediamtx.pid" ] && kill -0 "$(cat "$RUN_DIR/mediamtx.pid")" 2>/dev/null; then
    echo "running (pid $(cat "$RUN_DIR/mediamtx.pid"))"
  else
    echo "stopped"
  fi
}

case "${1:-}" in
  status)      status ;;
  server-start) server_start ;;
  server-stop)  server_stop ;;
  server-restart) server_stop; sleep 1; server_start ;;
  cam-start)   [ $# -ge 2 ] && cam_start "$2" || { echo "usage: $0 cam-start <cam>"; exit 1; } ;;
  cam-stop)    [ $# -ge 2 ] && cam_stop "$2" || { echo "usage: $0 cam-stop <cam>"; exit 1; } ;;
  cam-restart) [ $# -ge 2 ] && { cam_stop "$2"; sleep 1; cam_start "$2"; } \
                 || { echo "usage: $0 cam-restart <cam>"; exit 1; } ;;
  all-start)   server_start; sleep 1; for c in cam1 cam2 cam3 cam4; do cam_start "$c"; done ;;
  all-stop)    for c in cam4 cam3 cam2 cam1; do cam_stop "$c"; done; server_stop ;;
  *) echo "usage: $0 {status|server-start|server-stop|server-restart|cam-start|cam-stop|cam-restart|all-start|all-stop} [cam]"; exit 1 ;;
esac
