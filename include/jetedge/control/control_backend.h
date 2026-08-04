// ControlBackend — the control surface the Control API acts on (Stage 11).
//
// Implemented by pipeline::Pipeline.  The interface exists so the write-op
// flow (validate → safety check → snapshot → apply → audit → verify →
// rollback, CLAUDE.md §16) is unit-testable against a fake backend without
// GStreamer.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "jetedge/control/error_store.h"
#include "jetedge/metrics/metrics_registry.h"
#include "jetedge/pipeline/stream_config.h"
#include "jetedge/scheduler/scheduler_policy.h"

namespace jetedge {
namespace control {

// Which write operation is being requested (drives safety gating).
enum class WriteOp {
  kSetInferInterval,
  kSetPriority,
  kRestartStream,
  kRollback,
};

// Per-stream runtime status (one entry per stream, mux pad order).
struct StreamStatus {
  std::string stream_id;
  std::string type;            // "file" | "rtsp"
  std::string state;           // RTSP state (e.g. "RUNNING") or "RUNNING" for file
  std::string priority;        // "high" | "normal" | "low"
  int infer_interval = 0;
  uint64_t frames = 0;
  int reconnect_count = 0;     // RTSP only
  int consecutive_failures = 0;
  std::string last_reason;     // last reconnect/failure reason (RTSP only)
};

// Scheduler state snapshot for GET /scheduler/state.
struct SchedulerStatus {
  bool enabled = false;
  std::string state;           // "NORMAL" | "PRESSURE" | "THERMAL" | "CRITICAL" | "RECOVERY"
  int table_high = 0;          // current policy table
  int table_normal = 0;
  int table_low = 0;
  double cpu_pct = -1.0;
  double mem_pct = -1.0;
  double temp_c = -1.0;
  int adjustments = 0;         // escalations in the current window
  int max_adjustments = 0;
  int recovery_step = 0;
};

// One stream's entry inside a config snapshot.
struct StreamSnapshotEntry {
  std::string stream_id;
  pipeline::StreamPriority priority = pipeline::StreamPriority::kNormal;
  int infer_interval = 0;
};

// A full config snapshot: everything a rollback can restore.
struct ConfigSnapshot {
  std::string snapshot_id;
  uint64_t created_at_ms = 0;
  std::string reason;
  std::vector<StreamSnapshotEntry> streams;
  bool scheduler_enabled = false;
};

// Outcome of a backend write/verify operation.
struct WriteResult {
  bool success = false;
  std::string error_code;      // e.g. "RESTART_THROTTLED", "RTSP_FAILED"
  std::string detail;
};

class ControlBackend {
 public:
  virtual ~ControlBackend() = default;

  // Stream ids in mux pad order.
  virtual std::vector<std::string> stream_ids() const = 0;

  // Full runtime status (marshal-safe; called from control threads).
  virtual std::vector<StreamStatus> stream_status() const = 0;

  // Metrics snapshot (metrics registry is internally thread-safe).
  virtual std::vector<metrics::MetricsRegistry::StreamSummary> metrics_summary() const = 0;

  virtual SchedulerStatus scheduler_status() const = 0;
  virtual scheduler::SchedulerConfig scheduler_config() const = 0;

  // Recent recorded errors (GStreamer bus, RTSP watchdog, control ops),
  // newest first, at most `limit`.
  virtual std::vector<ErrorRecord> recent_errors(size_t limit) const = 0;

  // True when a load-increasing write op is currently allowed.  False when
  // the scheduler is in CRITICAL (CLAUDE.md §16: in CRITICAL neither the
  // scheduler nor the Agent may increase load).
  virtual bool safety_state_allows() const = 0;

  // ---- Write operations (each applies the change and is idempotent) --------

  virtual WriteResult set_infer_interval(int stream_idx, int interval) = 0;
  virtual WriteResult set_priority(int stream_idx, pipeline::StreamPriority priority) = 0;
  virtual WriteResult restart_stream(int stream_idx) = 0;

  // ---- Snapshot / rollback -------------------------------------------------

  // Full current effective configuration (for pre-change snapshots).
  virtual ConfigSnapshot current_config_snapshot() const = 0;

  // Restore every field from the snapshot.  Returns false only on a hard
  // failure (unknown stream id); rollback is best-effort otherwise.
  virtual WriteResult apply_snapshot(const ConfigSnapshot& snap) = 0;
};

}  // namespace control
}  // namespace jetedge
