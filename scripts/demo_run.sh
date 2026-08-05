#!/usr/bin/env bash
# demo_run.sh — Stage 15 P1: one-command demo orchestration.
#
# Chains the existing building blocks (rtsp_serve.sh + start_pipeline.sh +
# the web dashboard) so a full demo is ONE command, with guided steps.
#
#   demo_run.sh start            # file mode: 4 open-source videos, ~42 s run
#   demo_run.sh start --rtsp     # RTSP mode: MediaMTX + 4 published cameras
#                                #   (uses configs/streams_stage14_demo.yaml)
#   demo_run.sh start <config>   # explicit config
#   demo_run.sh stop             # graceful stop + cleanup
#   demo_run.sh status
#
# Dashboard: http://127.0.0.1:8091/dashboard on the Jetson, or through an
# SSH tunnel from any laptop (ssh -L 8091:127.0.0.1:8091 <jetson>).
# No sudo, no system packages. Reversible: stop restores the pre-demo state.

set -u

REPO="$(cd "$(dirname "$0")/.." && pwd)"
DEFAULT_CONFIG="configs/streams_openvideo.yaml"
RTSP_CONFIG="configs/streams_stage14_demo.yaml"
CTL_PORT="${CTL_PORT:-8091}"

usage() {
  sed -n '2,14p' "$0" | sed 's/^# \{0,1\}//'
}

start() {
  local config="$DEFAULT_CONFIG" rtsp_mode=0
  for a in "$@"; do
    case "$a" in
      --rtsp) rtsp_mode=1; config="$RTSP_CONFIG" ;;
      *) config="$a" ;;
    esac
  done

  if [ "$rtsp_mode" -eq 1 ]; then
    echo "[demo_run] starting RTSP serve (MediaMTX + 4 cameras)..."
    "$REPO/scripts/rtsp_serve.sh" server-start
    for cam in cam1 cam2 cam3 cam4; do
      "$REPO/scripts/rtsp_serve.sh" cam-start "$cam" || {
        echo "[demo_run] cam-start $cam failed; aborting" >&2; return 1; }
    done
  fi

  "$REPO/scripts/start_pipeline.sh" "$config" || return 1

  cat <<EOF

============== JetEdge-Agent DEMO ==============
  管道已启动（$config）
  仪表盘:  http://127.0.0.1:${CTL_PORT}/dashboard
  本机访问: ssh -L ${CTL_PORT}:127.0.0.1:${CTL_PORT} <jetson>  然后开浏览器

  演示步骤:
  1. 仪表盘实时看 4 路流 FPS / 延迟分位数 / 调度状态灯 / 事件流 / 关键帧
  2. 操作面板: 把某路 infer interval 改成 2 → 观察 FPS/延迟变化
     → 点"保存快照" → "回滚到最近快照" → 参数恢复、审计留痕
  3. (RTSP 模式) 停掉一路发布: scripts/rtsp_serve.sh cam-stop cam3
     → 看状态变 RECONNECTING → cam-start cam3 → 自动恢复 RUNNING
  4. 结束后: scripts/demo_run.sh stop
================================================
EOF
}

stop() {
  "$REPO/scripts/start_pipeline.sh" stop
  if [ -f "$REPO/scripts/rtsp_serve.sh" ]; then
    for cam in cam1 cam2 cam3 cam4; do
      "$REPO/scripts/rtsp_serve.sh" cam-stop "$cam" >/dev/null 2>&1 || true
    done
    "$REPO/scripts/rtsp_serve.sh" server-stop
  fi
  echo "[demo_run] stopped"
}

status() {
  "$REPO/scripts/start_pipeline.sh" status
  if [ -f "$REPO/scripts/rtsp_serve.sh" ]; then
    "$REPO/scripts/rtsp_serve.sh" status
  fi
}

case "${1:-}" in
  start)  shift; start "$@" ;;
  stop)   stop ;;
  status) status ;;
  *) usage ;;
esac
