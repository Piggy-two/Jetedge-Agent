// DecisionRouter — deterministic post-Qwen alert decision for the
// factory-intrusion scenario (2026-08-06).
//
// The Qwen visual review is best-effort; what happens AFTER the review must
// be deterministic local C++ logic (never LLM output).  This module classifies
// every routed zone_entry event into one of four alert tiers and appends one
// row to the incidents JSONL:
//
//   confirmed=true && confidence=high          -> confirmed_alert
//   confirmed=false && confidence=high         -> archived
//   confidence=medium                          -> manual_review
//   confidence=low                             -> archived
//   confidence missing / unknown               -> manual_review (fail-safe)
//   Qwen failed (HTTP / timeout / schema)      -> local_rule_only
//   not submitted (shed / circuit open / ...)  -> local_rule_only
//
// The Qwen schema has no numeric confidence: the requested "high && conf>=0.8"
// rule is adapted to the string grades above (documented deviation, see
// docs/scenario_factory_intrusion_report.md).
//
// Pure data types — no GStreamer/DeepStream deps, unit-testable directly.

#pragma once

#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include "jetedge/events/event_types.h"

namespace jetedge {
namespace llm {

// Alert tier produced for one event.
enum class Decision {
  kConfirmedAlert,  // Qwen confirmed, high confidence
  kManualReview,    // ambiguous / medium confidence / unknown confidence
  kArchived,        // low risk, logged only
  kLocalRuleOnly,   // cloud review unavailable — local rule stands
};

inline const char* decision_str(Decision d) {
  switch (d) {
    case Decision::kConfirmedAlert: return "confirmed_alert";
    case Decision::kManualReview:   return "manual_review";
    case Decision::kArchived:       return "archived";
    case Decision::kLocalRuleOnly:  return "local_rule_only";
  }
  return "unknown";
}

// Parsed Qwen visual review (success path only).
struct ReviewOutcome {
  bool confirmed = false;
  std::string summary;      // one sentence (informational only)
  std::string confidence;   // "high" | "medium" | "low" | "" (absent/unknown)
};

// How the cloud review attempt ended.
enum class QwenOutcome {
  kSucceeded,      // HTTP 200 + schema-valid response
  kFailed,         // HTTP error / timeout / schema-invalid
  kNotSubmitted,   // shed / circuit open / endpoint missing / key missing / ...
};

// Owns the incidents JSONL (append + flush per row).  Thread-safe for the
// event-probe thread (shed / not_routed path) and the LLM worker thread.
class DecisionRouter {
 public:
  DecisionRouter() = default;
  ~DecisionRouter();

  DecisionRouter(const DecisionRouter&) = delete;
  DecisionRouter& operator=(const DecisionRouter&) = delete;

  // Open the incidents JSONL in append mode.  No-op when `incidents_path` is
  // empty (feature disabled); decide() still returns a decision, writes nothing.
  bool init(const std::string& incidents_path,
            const std::vector<std::string>& stream_ids);
  void close();

  bool enabled() const { return jsonl_.is_open(); }

  // Defensive parse of the (already schema-validated) review JSON; tolerates
  // markdown fences and missing fields (defaults).  Returns false only when
  // the input is not parseable JSON at all.
  static bool parse_review(const std::string& json, ReviewOutcome& out);

  // Deterministic decision from the review.  Pure function.
  static Decision classify(const ReviewOutcome& o);

  // Record one incident row at the event's terminal outcome and return the
  // decision.  Never throws; when disabled, returns the decision only.
  Decision decide(const events::EventRecord& e, QwenOutcome outcome,
                  const ReviewOutcome& review, const std::string& detail);

  // Decisions recorded (rows appended when enabled).
  uint64_t written() const {
    std::lock_guard<std::mutex> lock(mu_);
    return written_;
  }

 private:
  void write_line(const events::EventRecord& e, Decision d,
                  const ReviewOutcome& r, const std::string& reason);
  std::string stream_id_at(int idx) const;
  static std::string sanitize_detail(const std::string& s);

  std::ofstream jsonl_;
  std::vector<std::string> stream_ids_;
  mutable std::mutex mu_;
  uint64_t written_ = 0;
};

}  // namespace llm
}  // namespace jetedge
