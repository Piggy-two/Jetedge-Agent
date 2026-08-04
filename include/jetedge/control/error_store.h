// ErrorStore — bounded ring buffer of recent pipeline/control errors (Stage 11).
//
// Fed by the pipeline (GStreamer bus ERROR, RTSP watchdog failures, control
// write-op failures) and served to GET /errors/recent.  Fixed capacity; the
// oldest record is overwritten when full, so memory stays bounded.

#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace jetedge {
namespace control {

struct ErrorRecord {
  uint64_t ts_ms = 0;
  std::string level;        // "ERROR" | "WARN"
  std::string stream_id;
  std::string state;        // e.g. "RECONNECTING", "running"
  std::string operation;
  std::string error_code;   // stable machine-readable code, e.g. "GST_ERROR"
  std::string message;
};

class ErrorStore {
 public:
  explicit ErrorStore(size_t capacity = 64);

  void add(ErrorRecord r);
  void clear();

  // Newest first, at most `limit` records (0 = all).
  std::vector<ErrorRecord> recent(size_t limit = 0) const;

  size_t capacity() const { return capacity_; }
  size_t size() const;

 private:
  const size_t capacity_;
  std::vector<ErrorRecord> records_;  // ring: push_back, erase front when full
  mutable std::mutex mu_;
};

}  // namespace control
}  // namespace jetedge
