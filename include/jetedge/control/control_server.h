// ControlServer — the safe Control API (Stage 11).
//
// Exposes a whitelisted HTTP surface over the ControlBackend (implemented by
// the pipeline).  Every write operation runs the CLAUDE.md §16 flow:
//
//   1. validate types and ranges
//   2. check the current safety state (CRITICAL blocks load increases)
//   3. save a pre-change snapshot
//   4. apply a bounded change
//   5. write an audit record
//   6. read back the applied value
//   7. on failure or verification mismatch: roll back automatically
//
// Write operations are serialized by a mutex (one at a time), so the
// "single request touches at most one stream" rule cannot be bypassed by
// concurrent requests.  The HTTP server runs on its own thread; a broken
// client or a hung request never touches the real-time pipeline.
//
// Endpoints:
//   GET  /dashboard                  → static web dashboard (Stage 15)
//   GET  /events/recent[?limit=N]    → newest-first events (bounded tail,
//                                      N clamped to [1,200], default 50)
//   GET  /keyframes                  → newest-first keyframe file names (≤100)
//   GET  /keyframes/<name>           → one keyframe JPEG (whitelisted name)
//   GET  /health                     → {"status":"ok"}
//   GET  /metrics/summary            → per-stream frame/FPS summary
//   GET  /streams                    → per-stream runtime status
//   GET  /streams/<id>               → one stream
//   GET  /scheduler/config           → scheduler configuration
//   GET  /scheduler/state            → scheduler state + last sample
//   GET  /errors/recent              → recent error records
//   POST /streams/<id>/infer-interval {"interval":0..max}   → write op
//   POST /streams/<id>/priority      {"priority":"high|normal|low"}
//   POST /streams/<id>/restart       {}   (RTSP only, throttled)
//   POST /config/snapshot            {"reason":"..."} → explicit snapshot
//   POST /config/rollback            {"snapshot_id":"..."} → restore
//   POST /benchmark {"duration_s":5..120, "per_stream":[...]} → measurement
//          window (read-only, single-flight; other requests queue while it
//          runs — the accept thread is blocked for duration_s)
//
// Response envelope:
//   {"success":true,  "request_id", "timestamp_ms", "data":{...}, "snapshot_id"}
//   {"success":false, "request_id", "timestamp_ms", "error_code", "error"}

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

#include <json/json.h>

#include "jetedge/control/audit_log.h"
#include "jetedge/control/control_backend.h"
#include "jetedge/control/control_config.h"
#include "jetedge/control/http_server.h"
#include "jetedge/control/snapshot_store.h"

namespace jetedge {
namespace control {

class ControlServer {
 public:
  ControlServer() = default;

  ControlServer(const ControlServer&) = delete;
  ControlServer& operator=(const ControlServer&) = delete;

  // Init snapshot store + audit log and start the HTTP listener.  Returns
  // false when the listener cannot be bound (caller logs and continues —
  // a Control API failure must never stop the pipeline).
  bool start(const ControlConfig& cfg, ControlBackend* backend);

  // Stop the HTTP listener and close the audit log.  Idempotent.
  void stop();

  bool running() const { return http_.running(); }
  int port() const { return http_.port(); }

  // Full routing + write-op flow, no sockets.  Public for unit tests.
  HttpResponse handle_request(const HttpRequest& req);

 private:
  // ---- routing -------------------------------------------------------------
  HttpResponse route(const HttpRequest& req);
  HttpResponse make_ok(const std::string& request_id, const Json::Value& data,
                       const std::string& snapshot_id = "");
  HttpResponse make_err(const std::string& request_id, int http_status,
                        const char* error_code, const std::string& message);

  // ---- read handlers -------------------------------------------------------
  HttpResponse serve_dashboard(const std::string& request_id);
  Json::Value json_health() const;
  Json::Value json_metrics_summary() const;
  Json::Value json_stream_status(const std::string& stream_id,
                                 bool* found) const;
  Json::Value json_scheduler_config() const;
  Json::Value json_scheduler_state() const;
  Json::Value json_recent_errors() const;
  Json::Value json_recent_events(const std::string& query) const;
  Json::Value json_keyframes() const;
  HttpResponse serve_keyframe(const std::string& request_id,
                              const std::string& name);

  // POST /benchmark — controlled measurement window (read-only, single-flight).
  HttpResponse handle_benchmark(const std::string& request_id, const Json::Value& body);

  // ---- write ops (the §16 flow) ---------------------------------------------
  HttpResponse write_op(const std::string& request_id, WriteOp op,
                        const std::string& stream_id, const Json::Value& body);
  HttpResponse handle_snapshot(const std::string& request_id, const Json::Value& body);
  HttpResponse handle_rollback(const std::string& request_id, const Json::Value& body);

  // Helpers shared by the write-op flow.
  bool capture_and_save_snapshot(const std::string& reason, std::string* snapshot_id);
  Json::Value stream_state_json(const std::vector<StreamStatus>& statuses,
                                int idx) const;
  void audit(const std::string& request_id, const std::string& operation,
             const std::string& stream_id, const Json::Value& args,
             const Json::Value& before, const Json::Value& after, bool success,
             const std::string& error_code, const std::string& snapshot_id);

  std::string next_request_id();
  std::string next_snapshot_id();

  ControlConfig cfg_;
  ControlBackend* backend_ = nullptr;
  HttpServer http_;
  SnapshotStore snapshots_;
  AuditLog audit_;
  std::mutex write_mu_;       // serializes write ops (one at a time)
  std::mutex benchmark_mu_;   // single-flight for POST /benchmark
  std::atomic<bool> stopping_{false};  // set before http_.stop(): lets a
                                       // running benchmark window exit early
  std::vector<uint64_t> last_restart_ms_;  // per-stream restart throttle (mono ms)
  uint64_t seq_ = 0;
};

}  // namespace control
}  // namespace jetedge
