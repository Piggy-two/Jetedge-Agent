// LLM data types for the Stage 7 async cloud-analysis module.
//
// Pure data types — no GStreamer/DeepStream/network dependencies, so the
// routing and queue logic is unit-testable directly.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace jetedge {
namespace llm {

// Cloud analysis providers.
enum class LlmProvider { kQwen, kDeepSeek };

inline const char* llm_provider_str(LlmProvider p) {
  switch (p) {
    case LlmProvider::kQwen:     return "qwen";
    case LlmProvider::kDeepSeek: return "deepseek";
  }
  return "unknown";
}

// Queue priority — lower value = higher priority.
enum class RequestPriority { kHigh = 0, kNormal = 1, kLow = 2 };

inline const char* priority_str(RequestPriority p) {
  switch (p) {
    case RequestPriority::kHigh:   return "high";
    case RequestPriority::kNormal: return "normal";
    case RequestPriority::kLow:    return "low";
  }
  return "unknown";
}

// One pending cloud-analysis request.  Either `event_type`/`stream_idx`/
// `orig_ts_ms` (Qwen visual review) or `metrics_json` (DeepSeek periodic
// diagnosis) is populated; the other side stays empty.
struct LlmRequest {
  LlmProvider provider = LlmProvider::kQwen;
  RequestPriority priority = RequestPriority::kNormal;
  uint64_t request_id = 0;   // monotonic sequence from the router

  // Qwen visual review
  std::string prompt;                   // built by PromptManager
  std::vector<std::string> image_paths; // full JPEG paths (may be empty)
  std::string event_type;               // "zone_entry", "appearance", ...
  int stream_idx = -1;
  uint64_t orig_ts_ms = 0;              // original event timestamp

  // DeepSeek metrics diagnosis
  std::string metrics_json;             // aggregated metrics payload

  // Routed away (request dropped without sending).  Logged, not fatal.
  bool shed = false;
};

// Result of one cloud-analysis attempt.
struct LlmResponse {
  LlmProvider provider = LlmProvider::kQwen;
  bool success = false;       // HTTP 200 + schema-valid response
  int http_status = 0;
  uint64_t latency_ms = 0;
  std::string result_json;    // parsed assistant content (JSON object)
  std::string error;          // error message when not successful
  uint64_t request_id = 0;
  std::string event_type;
  int stream_idx = -1;
  uint64_t orig_ts_ms = 0;
};

// One line of the cloud-analysis JSONL (schemas written back to disk).
struct CloudAnalysisRecord {
  uint64_t ts_ms = 0;         // when the result was written
  uint64_t request_id = 0;
  uint64_t orig_ts_ms = 0;
  std::string event_type;     // event that triggered the review, or "metrics"
  std::string stream_id;      // resolved stream id, or ""
  std::string provider;       // "qwen" / "deepseek"
  bool success = false;
  int http_status = 0;
  uint64_t latency_ms = 0;
  std::string result_json;    // raw parsed content (compact JSON)
  std::string error;          // error message when not successful
};

}  // namespace llm
}  // namespace jetedge
