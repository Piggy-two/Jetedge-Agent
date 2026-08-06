#!/usr/bin/env bash
# run_scenario_factory_intrusion.sh — factory dangerous-area intrusion
# scenario driver (2026-08-06).  Chains the phases below; each phase can also
# be run individually for a dry-run:
#
#   preflight publish start wait-ready baseline observe bench-during-qwen
#   agent fault soak stop validate summary
#
# Pipeline: build/jetedge_server with configs/scenario_factory_intrusion.yaml;
# RTSP sources via scripts/rtsp_serve_factory.sh; Control API + dashboard at
# 127.0.0.1:8091.  All artifacts land under logs/scenario_factory*.
#
# The scenario keeps its OWN server log (start_pipeline.sh's shared
# logs/jetedge_server.log stays untouched); decoder affinity comes from the
# sourced start_pipeline.sh functions.

set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO"
# shellcheck source=start_pipeline.sh
source scripts/start_pipeline.sh   # pin_decoder_threads + stop()

CONFIG="configs/scenario_factory_intrusion.yaml"
PORT=8091
API="http://127.0.0.1:$PORT"
LOGDIR="$REPO/logs"
SRV_LOG="$LOGDIR/scenario_factory_server.log"
STATE_FILE="$LOGDIR/.scenario_factory_state"
STABLE_CSV="$LOGDIR/scenario_factory_stability.csv"
FAULT_TIMELINE="$LOGDIR/scenario_factory_fault_timeline.jsonl"
SOAK_TOTAL_S=960          # >= 16 min process lifetime => >= 15 min RUNNING
BENCH_S=60                # baseline / during-Qwen benchmark windows

log()  { echo "[$(date '+%H:%M:%S')] $*"; }
api_get() { curl -sf --max-time 10 "$API$1" || true; }
save() { # <dir> <name> — snapshot one endpoint into the artifacts dir
  local dir="$1"
  local name="$2"
  local out="$dir/$name.json"
  mkdir -p "$(dirname "$out")"
  api_get "/$name" > "$out" || echo '{"error":"unavailable"}' > "$out"
  log "  saved $out ($(wc -c < "$out") bytes)"
}
save_state() { # <key> <value>
  grep -q "^$1=" "$STATE_FILE" 2>/dev/null && sed -i "s/^$1=.*/$1=$2/" "$STATE_FILE" \
    || echo "$1=$2" >> "$STATE_FILE"
}
load_state() { [ -f "$STATE_FILE" ] && . "$STATE_FILE" || true; }

# ---------------------------------------------------------------------------
phase_preflight() {
  log "== preflight =="
  [ -x build/jetedge_server ] || { log "FAIL: build/jetedge_server missing"; exit 1; }
  [ -f "$CONFIG" ] || { log "FAIL: $CONFIG missing"; exit 1; }
  for v in person-bicycle-car-detection.mp4 car-detection.mp4 \
           one-by-one-person-detection.mp4 people-detection.mp4; do
    [ -f "$HOME/jetedge-openvideos/$v" ] || { log "FAIL: missing $HOME/jetedge-openvideos/$v"; exit 1; }
  done
  if pgrep -x jetedge_server >/dev/null; then log "FAIL: jetedge_server already running"; exit 1; fi
  if ss -ltn 2>/dev/null | grep -q ":$PORT "; then log "FAIL: port $PORT busy"; exit 1; fi
  # Publisher collision guard (H7): refuse if a Stage 14/15 publish is active.
  local active
  active=$(scripts/rtsp_serve_factory.sh running-cams)
  if [ -n "$active" ]; then
    log "FAIL: cam publisher(s) already active: $active (stop them first: rtsp_serve.sh all-stop / rtsp_serve_factory.sh all-stop)"
    exit 1
  fi
  # Cloud keys: Qwen is REQUIRED (scenario evidence), DeepSeek warn-only.
  if [ -z "${QWEN_API_KEY:-}" ] && ! grep -qs '^QWEN_API_KEY=' "$HOME/.jetedge/secrets.env" 2>/dev/null; then
    log "FAIL: Qwen API key missing (env QWEN_API_KEY or ~/.jetedge/secrets.env) — scenario needs real Qwen rows"
    exit 1
  fi
  if [ -z "${DEEPSEEK_API_KEY:-}" ] && ! grep -qs '^DEEPSEEK_API_KEY=' "$HOME/.jetedge/secrets.env" 2>/dev/null; then
    log "WARN: DeepSeek key missing — the Agent will still run its deterministic fallback (audited)"
  fi
  # Every scenario artifact must come from THIS run only (no mixing of runs).
  rm -f "$LOGDIR/scenario_factory_detections.jsonl" "$LOGDIR/scenario_factory_events.jsonl"
  rm -f "$LOGDIR/scenario_factory_incidents.jsonl" "$LOGDIR/scenario_factory_analysis.jsonl"
  rm -f "$LOGDIR/scenario_factory_bench_during.json" "$LOGDIR/scenario_factory_stability.csv"
  rm -rf "$LOGDIR/keyframes_scenario_factory" "$LOGDIR/scenario_factory_baseline" \
         "$LOGDIR/scenario_factory_fault" "$LOGDIR/scenario_factory_agent" \
         "$LOGDIR/control_scenario_factory" "$LOGDIR/scenario_factory_server.log"
  rm -f "$STATE_FILE" "$FAULT_TIMELINE"
  log "preflight OK"
}

phase_publish() {
  log "== publish =="
  if [ -f "$HOME/jetedge-rtsp/run/mediamtx.pid" ] && \
     kill -0 "$(cat "$HOME/jetedge-rtsp/run/mediamtx.pid")" 2>/dev/null; then
    save_state mediamtx_started_by_us 0
    log "  mediamtx already running (left running afterwards)"
  else
    save_state mediamtx_started_by_us 1
  fi
  scripts/rtsp_serve_factory.sh all-start
  sleep 2
  scripts/rtsp_serve_factory.sh status
}

phase_start() {
  log "== start pipeline =="
  nohup build/jetedge_server "$CONFIG" > "$SRV_LOG" 2>&1 &
  local pid=$!
  save_state server_pid "$pid"
  save_state start_epoch "$(date +%s)"
  log "  jetedge_server pid=$pid log=$SRV_LOG"
  # Pin decoder threads once src-camN-decode threads appear (Stage 10).
  for _ in $(seq 1 6); do
    sleep 5
    if ls /proc/$pid/task/*/comm 2>/dev/null | xargs grep -lE 'src-cam[1-4]-decode' 2>/dev/null | wc -l | grep -qE '[4-9]'; then
      pin_decoder_threads "$pid"
      log "  decode threads pinned"
      return 0
    fi
  done
  log "  WARN: decode threads not fully detected after 30s; pin skipped"
  return 0
}

phase_wait_ready() {
  log "== wait-ready =="
  for _ in $(seq 1 24); do
    local n
    n=$(api_get "/streams" | python3 -c "import sys,json; d=json.load(sys.stdin); print(sum(1 for s in d.get('data',{}).get('streams',[]) if s.get('state')=='RUNNING'))" 2>/dev/null || echo 0)
    if [ "$n" = "4" ]; then log "  all 4 streams RUNNING"; return 0; fi
    sleep 5
  done
  log "FAIL: streams not all RUNNING within 120s (last=$(api_get /streams | head -c 400))"
  exit 1
}

phase_baseline() {
  log "== baseline =="
  local dir="$LOGDIR/scenario_factory_baseline"
  mkdir -p "$dir"
  save "$dir" streams
  save "$dir" metrics/summary
  save "$dir" scheduler/state
  log "  pre-Qwen benchmark (${BENCH_S}s)..."
  curl -sf --max-time 150 -X POST "$API/benchmark" -d "{\"duration_s\":$BENCH_S}" \
    > "$dir/bench_before.json" || log "  WARN benchmark failed"
  log "  benchmark saved"
  # RSS/CPU/temp soak sampler runs for the whole scenario.
  nohup python3 scripts/stability_monitor.py --pid "$(load_state; echo "$server_pid")" \
    --csv "$STABLE_CSV" --interval 10 --max-duration 1500 --api "$API" \
    > "$LOGDIR/scenario_factory_stability_monitor.log" 2>&1 &
  save_state monitor_pid "$!"
  log "  stability monitor started"
}

phase_observe() {
  log "== observe (180s: track continuity + zone_entry + Qwen review) =="
  sleep 180
  python3 - <<'EOF'
import json
ev = "logs/scenario_factory_events.jsonl"
ze = {}
try:
    for line in open(ev):
        d = json.loads(line)
        if d.get("event") == "zone_entry":
            ze.setdefault((d["stream_id"], d["track_id"], d.get("zone")), []).append(d)
except FileNotFoundError:
    pass
print(f"zone_entry dedup check: {len(ze)} distinct (stream, track, zone) keys; "
      f"max fires per key = {max((len(v) for v in ze.values()), default=0)}")
EOF
  log "  zone_entry rows: $(grep -c '"zone_entry"' "$LOGDIR/scenario_factory_events.jsonl" 2>/dev/null || echo 0)"
  log "  keyframes: $(ls "$LOGDIR/keyframes_scenario_factory/" 2>/dev/null | wc -l)"
  log "  analysis rows (qwen): $(grep -c '"provider":"qwen"' "$LOGDIR/scenario_factory_analysis.jsonl" 2>/dev/null || echo 0)"
  log "  incidents rows: $(grep -c . "$LOGDIR/scenario_factory_incidents.jsonl" 2>/dev/null || echo 0)"
}

phase_bench_during_qwen() {
  log "== benchmark during active Qwen traffic =="
  curl -sf --max-time 150 -X POST "$API/benchmark" -d "{\"duration_s\":$BENCH_S}" \
    > "$LOGDIR/scenario_factory_bench_during.json" || log "  WARN benchmark failed"
  log "  saved scenario_factory_bench_during.json"
}

phase_agent() {
  log "== python runtime Agent (safe optimization loop) =="
  mkdir -p "$LOGDIR/scenario_factory_agent"
  local rc
  set +e
  python3 agent/main.py \
    --goal "保证 cam1 推理 FPS 不低于 10，降低全局 P95 延迟" \
    --config agent/config_scenario_factory.yaml \
    --base-url "$API" --benchmark-duration 30 \
    > "$LOGDIR/scenario_factory_agent/run.log" 2>&1
  rc=$?
  set -e
  save_state agent_rc "$rc"
  log "  agent exit code = $rc (0=kept, 1=rollback/not-met — both are valid safe-loop outcomes)"
  log "  agent audit: $(grep -c . "$LOGDIR/scenario_factory_agent/audit.jsonl" 2>/dev/null || echo 0) rows"
  log "  agent report: $(ls "$LOGDIR/scenario_factory_agent/reports/" 2>/dev/null | tail -1)"
}

phase_fault() {
  log "== cam4 fault injection =="
  local dir="$LOGDIR/scenario_factory_fault"
  mkdir -p "$dir"
  # Distinct snapshot names — the three phases must not overwrite each other.
  save "$dir" streams && mv "$dir/streams.json" "$dir/fault_before.json"
  scripts/rtsp_serve_factory.sh cam-stop cam4
  log "  cam4 publish stopped — sampling /streams every 2s for 16s"
  log "  (measured: retry budget 5 exhausts in ~35s → FAILED is terminal by design,"
  log "   no more automatic attempts; the window keeps cam4 in RECONNECTING"
  log "   so auto-recovery after restore is observable)"
  python3 - > "$FAULT_TIMELINE" <<'EOF'
import json, sys, time, urllib.request
base = "http://127.0.0.1:8091"
t0 = time.time()
for i in range(8):
    try:
        with urllib.request.urlopen(base + "/streams", timeout=5) as r:
            d = json.load(r)
        row = {"t_s": round(time.time() - t0, 1)}
        for s in d.get("data", {}).get("streams", []):
            row[s["stream_id"]] = s.get("state")
            row[s["stream_id"] + "_reconnects"] = s.get("reconnect_count", 0)
            row[s["stream_id"] + "_failures"] = s.get("consecutive_failures", 0)
        print(json.dumps(row), flush=True)
    except Exception as e:
        print(json.dumps({"t_s": round(time.time() - t0, 1), "error": str(e)}), flush=True)
    time.sleep(2)
EOF
  log "  fault timeline rows: $(grep -c . "$FAULT_TIMELINE")"
  save "$dir" streams && mv "$dir/streams.json" "$dir/fault_mid.json"
  scripts/rtsp_serve_factory.sh cam-start cam4
  log "  cam4 publish restored — waiting for RUNNING (auto-recovery from RECONNECTING)"
  local ok=0
  for _ in $(seq 1 12); do
    local st
    st=$(api_get "/streams" | python3 -c "import sys,json; print(next((s.get('state') for s in json.load(sys.stdin).get('data',{}).get('streams',[]) if s.get('stream_id')=='cam4'),'?'))" 2>/dev/null || echo '?')
    if [ "$st" = "RUNNING" ]; then log "  cam4 RUNNING"; ok=1; break; fi
    sleep 5
  done
  if [ "$ok" != "1" ]; then
    log "FAIL: cam4 did not return to RUNNING within 60s"
    save "$dir" streams && mv "$dir/streams.json" "$dir/fault_after.json"
    exit 1
  fi
  save "$dir" streams && mv "$dir/streams.json" "$dir/fault_after.json"
  log "  snapshots saved under $dir/ (fault_before/mid/after)"
}

phase_soak() {
  load_state
  local elapsed run_sec
  elapsed=$(($(date +%s) - start_epoch))
  run_sec=$((SOAK_TOTAL_S - elapsed))
  if [ "$run_sec" -gt 0 ]; then
    log "== soak: pipeline runtime ${elapsed}s, sleeping ${run_sec}s to reach ${SOAK_TOTAL_S}s =="
    sleep "$run_sec"
  else
    log "== soak: already at ${elapsed}s (>= ${SOAK_TOTAL_S}s) =="
  fi
  load_state
  if [ -n "${monitor_pid:-}" ] && kill -0 "$monitor_pid" 2>/dev/null; then
    log "  stopping stability monitor"
    kill "$monitor_pid" 2>/dev/null || true
  fi
  log "  stability CSV rows: $(grep -c . "$STABLE_CSV" 2>/dev/null || echo 0)"
}

phase_stop() {
  log "== stop =="
  stop || log "  WARN: stop reported non-zero (slow shutdown)"  # SIGINT + wait
  scripts/rtsp_serve_factory.sh all-stop
  load_state
  if [ "${mediamtx_started_by_us:-0}" = "1" ]; then
    scripts/rtsp_serve.sh server-stop
    log "  mediamtx stopped (we started it)"
  else
    log "  mediamtx left running (pre-existing)"
  fi
  local rc
  rc=$(grep -c . "$SRV_LOG" 2>/dev/null || echo 0)
  log "  server log lines: $rc; exit path: $(tail -3 "$SRV_LOG" | tr '\n' ' ')"
}

phase_validate() {
  log "== validate =="
  python3 scripts/validate_jsonl.py \
    --fields ts_ms,stream_id,frame_num,track_id,class_id,class,confidence,bbox \
    "$LOGDIR/scenario_factory_detections.jsonl"
  python3 scripts/validate_jsonl.py \
    --fields ts_ms,stream_id,frame_num,event,class_id,class,track_id,bbox,count,zone,keyframe \
    "$LOGDIR/scenario_factory_events.jsonl"
  python3 scripts/validate_jsonl.py \
    --fields ts_ms,request_id,event_type,stream_id,provider,success,http_status,latency_ms,result,error \
    "$LOGDIR/scenario_factory_analysis.jsonl"
  python3 scripts/validate_jsonl.py \
    --fields event_id,stream_id,track_id,local_event,qwen_description,qwen_risk,qwen_confidence,decision,timestamp \
    "$LOGDIR/scenario_factory_incidents.jsonl"
  python3 scripts/validate_jsonl.py --fields ts_ms,request_id,operation \
    "$LOGDIR/control_scenario_factory/audit.jsonl"
  python3 scripts/validate_jsonl.py --fields ts_ms,run_id,phase,ok \
    "$LOGDIR/scenario_factory_agent/audit.jsonl"
  log "  semantic checks..."
  python3 scripts/verify_factory_scenario.py
  log "  process cleanup check..."
  if pgrep -x jetedge_server >/dev/null; then log "  FAIL: jetedge_server still running"; exit 1; fi
  if pgrep -x ffmpeg >/dev/null; then log "  WARN: ffmpeg processes remain: $(pgrep -x ffmpeg | tr '\n' ' ')"; else log "  OK: no ffmpeg left"; fi
  if [ -f "$HOME/jetedge-rtsp/run-factory" ] && ls "$HOME/jetedge-rtsp/run-factory/"*.pid >/dev/null 2>&1; then
    log "  WARN: factory publisher pids remain"; else log "  OK: no factory publisher pids"; fi
}

phase_summary() {
  log "== summary =="
  python3 - <<'EOF'
import json, os
L = "logs"
rows = [
    ("detections",  f"{L}/scenario_factory_detections.jsonl"),
    ("events",      f"{L}/scenario_factory_events.jsonl"),
    ("analysis",    f"{L}/scenario_factory_analysis.jsonl"),
    ("incidents",   f"{L}/scenario_factory_incidents.jsonl"),
    ("control audit", f"{L}/control_scenario_factory/audit.jsonl"),
    ("agent audit", f"{L}/scenario_factory_agent/audit.jsonl"),
]
for name, p in rows:
    n = 0
    try:
        with open(p) as f: n = sum(1 for _ in f)
    except FileNotFoundError: pass
    print(f"  {name:14s} {n:8d} lines  {p}")
kf = "logs/keyframes_scenario_factory"
print(f"  keyframes      {len(os.listdir(kf)) if os.path.isdir(kf) else 0:8d} files  {kf}/")
dec = {}
try:
    for line in open(f"{L}/scenario_factory_incidents.jsonl"):
        d = json.loads(line); dec[d["decision"]] = dec.get(d["decision"], 0) + 1
except FileNotFoundError: pass
print("  incident decisions:", dec if dec else "(none)")
state = {}
if os.path.exists(f"{L}/.scenario_factory_state"):
    for line in open(f"{L}/.scenario_factory_state"):
        if "=" in line:
            k, v = line.rstrip().split("=", 1); state[k] = v
print("  agent exit code:", state.get("agent_rc", "n/a"))
EOF
}

phase_run() {
  phase_preflight
  phase_publish
  phase_start
  phase_wait_ready
  phase_baseline
  phase_observe
  phase_bench_during_qwen
  phase_agent
  phase_fault
  phase_soak
  phase_stop
  phase_validate
  phase_summary
  log "== scenario run complete =="
}

if [ $# -eq 0 ]; then
  echo "usage: $0 {run|<phase>}   phases: preflight publish start wait-ready baseline observe bench-during-qwen agent fault soak stop validate summary"
  exit 1
fi

case "$1" in
  wait-ready)        phase_wait_ready ;;
  bench-during-qwen) phase_bench_during_qwen ;;
  *)                 "phase_$1" ;;
esac
