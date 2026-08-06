// DecisionRouter implementation — see decision_router.h.

#include "jetedge/llm/decision_router.h"

#include <algorithm>
#include <ctime>

#include <json/json.h>

#include "jetedge/common/logging.h"
#include "jetedge/llm/prompt_manager.h"

namespace jetedge {
namespace llm {

namespace {

uint64_t now_ms() {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000ULL +
         static_cast<uint64_t>(ts.tv_nsec / 1000000);
}

}  // namespace

DecisionRouter::~DecisionRouter() {
  close();
}

bool DecisionRouter::init(const std::string& incidents_path,
                          const std::vector<std::string>& stream_ids) {
  stream_ids_ = stream_ids;
  if (incidents_path.empty()) {
    LOG_INFO("llm", "decision router disabled (incidents_path empty)");
    return true;
  }
  jsonl_.open(incidents_path, std::ios::out | std::ios::app);
  if (!jsonl_.is_open()) {
    LOG_WARN("llm", "incidents JSONL open failed: %s — decisions logged only",
             incidents_path.c_str());
    return false;
  }
  LOG_INFO("llm", "decision router active, incidents JSONL: %s",
           incidents_path.c_str());
  return true;
}

void DecisionRouter::close() {
  std::lock_guard<std::mutex> lock(mu_);
  if (jsonl_.is_open()) {
    jsonl_.flush();
    jsonl_.close();
  }
}

bool DecisionRouter::parse_review(const std::string& json,
                                  ReviewOutcome& out) {
  out = ReviewOutcome{};
  const std::string cleaned = PromptManager::strip_markdown_fence(json);
  Json::Value root;
  Json::Reader reader;
  if (!reader.parse(cleaned, root) || !root.isObject()) {
    return false;
  }
  if (root.isMember("confirmed") && root["confirmed"].isBool()) {
    out.confirmed = root["confirmed"].asBool();
  }
  if (root.isMember("summary") && root["summary"].isString()) {
    out.summary = root["summary"].asString();
  }
  if (root.isMember("confidence") && root["confidence"].isString()) {
    out.confidence = root["confidence"].asString();
  }
  return true;
}

Decision DecisionRouter::classify(const ReviewOutcome& o) {
  if (o.confidence == "high") {
    return o.confirmed ? Decision::kConfirmedAlert : Decision::kArchived;
  }
  if (o.confidence == "medium") {
    return Decision::kManualReview;
  }
  if (o.confidence == "low") {
    return Decision::kArchived;
  }
  // Missing or unknown confidence: fail-safe escalation, never silent archive.
  return Decision::kManualReview;
}

Decision DecisionRouter::decide(const events::EventRecord& e,
                                QwenOutcome outcome,
                                const ReviewOutcome& review,
                                const std::string& detail) {
  Decision d;
  std::string reason;
  switch (outcome) {
    case QwenOutcome::kSucceeded:
      d = classify(review);
      switch (d) {
        case Decision::kConfirmedAlert: reason = "confirmed_high"; break;
        case Decision::kManualReview:
          reason = (review.confidence.empty() || review.confidence != "medium")
                       ? "unknown_confidence"
                       : "medium";
          break;
        case Decision::kArchived:
          reason = review.confidence == "low" ? "low_confidence"
                                              : "not_confirmed_high";
          break;
        default: reason = "unclassified"; break;
      }
      break;
    case QwenOutcome::kFailed:
      d = Decision::kLocalRuleOnly;
      reason = "qwen_failed:" + sanitize_detail(detail);
      break;
    case QwenOutcome::kNotSubmitted:
      d = Decision::kLocalRuleOnly;
      reason = "not_submitted:" + sanitize_detail(detail);
      break;
  }

  write_line(e, d, review, reason);
  return d;
}

void DecisionRouter::write_line(const events::EventRecord& e, Decision d,
                                const ReviewOutcome& r,
                                const std::string& reason) {
  const std::string sid = stream_id_at(e.stream_idx);
  // The event JSONL has no event id; synthesize a deterministic one from the
  // (ts_ms, stream, track, zone) key — zone_entry fires once per (track, zone).
  const std::string event_id =
      "evt_" + std::to_string(e.ts_ms) + "_" + sid + "_" +
      std::to_string(e.track_id) + "_" +
      (e.zone.empty() ? "nozone" : e.zone);

  Json::Value rec;
  rec["event_id"] = event_id;
  rec["stream_id"] = sid;
  rec["track_id"] = Json::Value::UInt64(e.track_id);
  rec["local_event"] = events::event_type_str(e.type);
  rec["zone"] = e.zone;
  rec["bbox"] = Json::Value(Json::arrayValue);
  rec["bbox"].append(Json::Value(e.left));
  rec["bbox"].append(Json::Value(e.top));
  rec["bbox"].append(Json::Value(e.width));
  rec["bbox"].append(Json::Value(e.height));
  rec["local_confidence"] = e.confidence;
  rec["qwen_description"] = r.summary;
  rec["qwen_risk"] = d == Decision::kLocalRuleOnly
                         ? "unknown"
                         : (r.confirmed ? "high" : "low");
  rec["qwen_confidence"] = r.confidence;
  rec["decision"] = decision_str(d);
  rec["reason"] = reason;
  rec["timestamp"] = Json::Value::Int64(static_cast<Json::Int64>(now_ms()));

  Json::FastWriter writer;
  const std::string line = writer.write(rec);

  std::lock_guard<std::mutex> lock(mu_);
  if (jsonl_.is_open()) {
    jsonl_ << line;
    jsonl_.flush();
  }
  ++written_;
}

std::string DecisionRouter::stream_id_at(int idx) const {
  if (idx >= 0 && idx < static_cast<int>(stream_ids_.size())) {
    return stream_ids_[static_cast<size_t>(idx)];
  }
  return "unknown";
}

std::string DecisionRouter::sanitize_detail(const std::string& s) {
  std::string out = s;
  out.erase(std::remove_if(out.begin(), out.end(),
                           [](unsigned char c) { return c == '\n' || c == '\r'; }),
            out.end());
  if (out.size() > 160) {
    out.resize(160);
  }
  if (out.empty()) {
    out = "unspecified";
  }
  return out;
}

}  // namespace llm
}  // namespace jetedge
