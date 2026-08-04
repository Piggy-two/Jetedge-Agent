// AuditLog — JSONL append-only audit trail for Control API write ops (Stage 11).
//
// Every write op appends one line: request_id, operation, stream, args,
// before/after state, snapshot_id, result.  The file lives under the control
// state dir (runtime data, never committed).  Append is atomic for a single
// record (single fwrite under the line's mutex).

#pragma once

#include <cstdint>
#include <mutex>
#include <string>

namespace jetedge {
namespace control {

struct AuditRecord {
  uint64_t ts_ms = 0;
  std::string request_id;
  std::string operation;     // e.g. "set_infer_interval"
  std::string stream_id;
  std::string args;          // JSON-encoded request arguments
  std::string before;        // JSON-encoded pre-change state (per stream)
  std::string after;         // JSON-encoded post-change state (per stream)
  bool success = false;
  std::string error_code;    // empty on success
  std::string snapshot_id;
};

class AuditLog {
 public:
  // Open (or create) the JSONL file in append mode.  Returns false when the
  // file cannot be opened (audit failures are logged, never fatal).
  bool open(const std::string& path);

  // Append one record.  Returns false on write failure.
  bool append(const AuditRecord& r);

  void close();

  bool is_open() const { return fp_ != nullptr; }

 private:
  std::string path_;
  std::mutex mu_;
  void* fp_ = nullptr;  // FILE*, kept void* to avoid stdio noise in the header
};

}  // namespace control
}  // namespace jetedge
