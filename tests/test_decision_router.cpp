// Unit tests for DecisionRouter (factory-intrusion scenario, 2026-08-06).
// Pure logic — no GStreamer/DeepStream dependency.

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include <json/json.h>

#include "jetedge/llm/decision_router.h"

using jetedge::events::EventRecord;
using jetedge::events::EventType;
using jetedge::llm::Decision;
using jetedge::llm::DecisionRouter;
using jetedge::llm::QwenOutcome;
using jetedge::llm::ReviewOutcome;

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond)                                                     \
  do {                                                                  \
    ++g_checks;                                                         \
    if (!(cond)) {                                                      \
      std::printf("FAIL line %d: %s\n", __LINE__, #cond);               \
      ++g_failures;                                                     \
    }                                                                   \
  } while (0)

namespace {

std::string temp_incidents_path() {
  return std::string("/tmp/jetedge_test_incidents_") +
         std::to_string(static_cast<long>(getpid())) + ".jsonl";
}

EventRecord zone_event() {
  EventRecord e;
  e.type = EventType::kZoneEntry;
  e.stream_idx = 2;  // cam3
  e.frame_num = 1234;
  e.ts_ms = 1785900000123ULL;
  e.track_id = 42;
  e.class_id = 0;  // person
  e.confidence = 0.81f;
  e.left = 600.0f;
  e.top = 280.0f;
  e.width = 120.0f;
  e.height = 200.0f;
  e.zone = "restricted_zone";
  return e;
}

// Read every non-empty line of a JSONL file as Json::Value.
std::vector<Json::Value> read_lines(const std::string& path) {
  std::vector<Json::Value> out;
  std::ifstream f(path);
  std::string line;
  while (std::getline(f, line)) {
    if (line.empty() || line == "\n") continue;
    Json::Value v;
    Json::Reader r;
    CHECK(r.parse(line, v));  // every line must be valid JSON
    out.push_back(v);
  }
  return out;
}

// Structural checks only (no per-line value assertions — the concurrency
// test writes lines with many different track_ids).
void check_required_fields(const Json::Value& v) {
  CHECK(v.isObject());
  CHECK(v.isMember("event_id"));
  CHECK(v.isMember("stream_id"));
  CHECK(v.isMember("track_id"));
  CHECK(v.isMember("local_event"));
  CHECK(v.isMember("qwen_description"));
  CHECK(v.isMember("qwen_risk"));
  CHECK(v.isMember("qwen_confidence"));
  CHECK(v.isMember("decision"));
  CHECK(v.isMember("timestamp"));
  CHECK(v["local_event"].asString() == "zone_entry");
  CHECK(v["stream_id"].asString() == "cam3");
}

}  // namespace

// ---- parse_review -----------------------------------------------------------

static void test_parse_valid() {
  ReviewOutcome o;
  CHECK(DecisionRouter::parse_review(
      R"({"confirmed":true,"summary":"person at entrance","confidence":"high"})",
      o));
  CHECK(o.confirmed == true);
  CHECK(o.summary == "person at entrance");
  CHECK(o.confidence == "high");
}

static void test_parse_markdown_fence() {
  ReviewOutcome o;
  CHECK(DecisionRouter::parse_review(
      "```json\n{\"confirmed\":false,\"summary\":\"s\",\"confidence\":\"low\"}\n```",
      o));
  CHECK(o.confirmed == false);
  CHECK(o.confidence == "low");
}

static void test_parse_invalid_json() {
  ReviewOutcome o;
  CHECK(!DecisionRouter::parse_review("not json at all", o));
  CHECK(!DecisionRouter::parse_review("", o));
}

static void test_parse_missing_fields_default() {
  ReviewOutcome o;
  CHECK(DecisionRouter::parse_review("{}", o));
  CHECK(o.confirmed == false);
  CHECK(o.summary.empty());
  CHECK(o.confidence.empty());
}

static void test_parse_non_bool_confirmed() {
  ReviewOutcome o;
  // Schema-invalid at the prompt-manager layer, but the router must not crash.
  CHECK(DecisionRouter::parse_review(R"({"confirmed":"yes","confidence":"high"})", o));
  CHECK(o.confirmed == false);
  CHECK(o.confidence == "high");
}

static void test_parse_unknown_confidence_kept() {
  ReviewOutcome o;
  CHECK(DecisionRouter::parse_review(R"({"confirmed":true,"confidence":"very_high"})", o));
  CHECK(o.confidence == "very_high");  // classifier handles unknown strings
}

// ---- classify (rule table) --------------------------------------------------

static void test_classify_table() {
  ReviewOutcome o;
  o.confirmed = true;  o.confidence = "high";
  CHECK(DecisionRouter::classify(o) == Decision::kConfirmedAlert);

  o.confirmed = false; o.confidence = "high";
  CHECK(DecisionRouter::classify(o) == Decision::kArchived);

  o.confirmed = true;  o.confidence = "medium";
  CHECK(DecisionRouter::classify(o) == Decision::kManualReview);

  o.confirmed = false; o.confidence = "medium";
  CHECK(DecisionRouter::classify(o) == Decision::kManualReview);

  o.confirmed = true;  o.confidence = "low";
  CHECK(DecisionRouter::classify(o) == Decision::kArchived);

  o.confirmed = false; o.confidence = "low";
  CHECK(DecisionRouter::classify(o) == Decision::kArchived);

  o.confirmed = true;  o.confidence = "";
  CHECK(DecisionRouter::classify(o) == Decision::kManualReview);

  o.confirmed = false; o.confidence = "";
  CHECK(DecisionRouter::classify(o) == Decision::kManualReview);

  o.confirmed = true;  o.confidence = "very_high";
  CHECK(DecisionRouter::classify(o) == Decision::kManualReview);
}

// ---- decide + incidents JSONL ----------------------------------------------

static void test_decide_success_paths() {
  const std::string path = temp_incidents_path();
  std::remove(path.c_str());
  DecisionRouter r;
  CHECK(r.init(path, {"cam1", "cam2", "cam3", "cam4"}));

  ReviewOutcome o;
  o.confirmed = true; o.summary = "person inside restricted zone";
  o.confidence = "high";
  CHECK(r.decide(zone_event(), QwenOutcome::kSucceeded, o, "") ==
        Decision::kConfirmedAlert);
  CHECK(r.written() == 1);

  o.confirmed = true; o.confidence = "medium";
  CHECK(r.decide(zone_event(), QwenOutcome::kSucceeded, o, "") ==
        Decision::kManualReview);

  o.confirmed = true; o.confidence = "low";
  CHECK(r.decide(zone_event(), QwenOutcome::kSucceeded, o, "") ==
        Decision::kArchived);

  o.confirmed = false; o.confidence = "high";
  CHECK(r.decide(zone_event(), QwenOutcome::kSucceeded, o, "") ==
        Decision::kArchived);

  o.confirmed = true; o.confidence = "";
  CHECK(r.decide(zone_event(), QwenOutcome::kSucceeded, o, "") ==
        Decision::kManualReview);
  CHECK(r.written() == 5);

  r.close();
  const auto lines = read_lines(path);
  CHECK(lines.size() == 5);
  check_required_fields(lines[0]);
  // Value-level checks on the first (default track_id=42) line only.
  CHECK(lines[0]["track_id"].asUInt64() == 42);
  CHECK(lines[0]["event_id"].asString().find("evt_1785900000123_cam3_42_") == 0);
  CHECK(lines[0]["decision"].asString() == "confirmed_alert");
  CHECK(lines[0]["reason"].asString() == "confirmed_high");
  CHECK(lines[0]["qwen_risk"].asString() == "high");
  CHECK(lines[0]["qwen_confidence"].asString() == "high");
  CHECK(lines[0]["qwen_description"].asString() == "person inside restricted zone");
  CHECK(lines[1]["reason"].asString() == "medium");
  CHECK(lines[2]["reason"].asString() == "low_confidence");
  CHECK(lines[3]["reason"].asString() == "not_confirmed_high");
  CHECK(lines[3]["qwen_risk"].asString() == "low");
  CHECK(lines[4]["reason"].asString() == "unknown_confidence");
  CHECK(lines[4]["decision"].asString() == "manual_review");
  std::remove(path.c_str());
}

static void test_decide_failure_and_not_submitted() {
  const std::string path = temp_incidents_path();
  std::remove(path.c_str());
  DecisionRouter r;
  CHECK(r.init(path, {"cam1", "cam2", "cam3", "cam4"}));

  CHECK(r.decide(zone_event(), QwenOutcome::kFailed, {},
                 "HTTP 500") == Decision::kLocalRuleOnly);
  CHECK(r.decide(zone_event(), QwenOutcome::kFailed, {},
                 "timeout after 30s\nretries exhausted") ==
        Decision::kLocalRuleOnly);
  CHECK(r.decide(zone_event(), QwenOutcome::kFailed, {},
                 "schema validation failed") == Decision::kLocalRuleOnly);
  CHECK(r.decide(zone_event(), QwenOutcome::kNotSubmitted, {},
                 "circuit_open") == Decision::kLocalRuleOnly);
  CHECK(r.decide(zone_event(), QwenOutcome::kNotSubmitted, {},
                 "queue_shed") == Decision::kLocalRuleOnly);
  CHECK(r.decide(zone_event(), QwenOutcome::kNotSubmitted, {},
                 "api_key_missing") == Decision::kLocalRuleOnly);
  CHECK(r.written() == 6);

  r.close();
  const auto lines = read_lines(path);
  CHECK(lines.size() == 6);
  for (const auto& l : lines) {
    check_required_fields(l);
    CHECK(l["decision"].asString() == "local_rule_only");
    CHECK(l["qwen_risk"].asString() == "unknown");
  }
  CHECK(lines[0]["reason"].asString() == "qwen_failed:HTTP 500");
  // Newlines sanitized out of the reason; line stays single-line JSON.
  CHECK(lines[1]["reason"].asString() == "qwen_failed:timeout after 30sretries exhausted");
  CHECK(lines[2]["reason"].asString() == "qwen_failed:schema validation failed");
  CHECK(lines[3]["reason"].asString() == "not_submitted:circuit_open");
  CHECK(lines[4]["reason"].asString() == "not_submitted:queue_shed");
  CHECK(lines[5]["reason"].asString() == "not_submitted:api_key_missing");
  std::remove(path.c_str());
}

static void test_decide_disabled_no_file() {
  const std::string path = "/tmp/jetedge_test_disabled_never_created.jsonl";
  std::remove(path.c_str());
  DecisionRouter r;  // never init()ed
  CHECK(!r.enabled());
  ReviewOutcome o;
  o.confirmed = true; o.confidence = "high";
  // Decision still computed and returned; nothing written, no crash.
  CHECK(r.decide(zone_event(), QwenOutcome::kSucceeded, o, "") ==
        Decision::kConfirmedAlert);
  CHECK(r.written() == 1);
  std::ifstream f(path);
  CHECK(!f.good());  // no file created
  std::remove(path.c_str());
}

static void test_decide_stream_idx_out_of_range() {
  const std::string path = temp_incidents_path();
  std::remove(path.c_str());
  DecisionRouter r;
  CHECK(r.init(path, {"cam1"}));
  EventRecord e = zone_event();
  e.stream_idx = 99;
  ReviewOutcome o;
  o.confirmed = true; o.confidence = "high";
  r.decide(e, QwenOutcome::kSucceeded, o, "");
  r.close();
  const auto lines = read_lines(path);
  CHECK(lines.size() == 1);
  CHECK(lines[0]["stream_id"].asString() == "unknown");
  std::remove(path.c_str());
}

static void test_decide_concurrent_integrity() {
  const std::string path = temp_incidents_path();
  std::remove(path.c_str());
  auto r = std::make_shared<DecisionRouter>();
  CHECK(r->init(path, {"cam1", "cam2", "cam3", "cam4"}));

  constexpr int kPerThread = 50;
  auto worker = [r](int base) {
    for (int i = 0; i < kPerThread; ++i) {
      EventRecord e = zone_event();
      e.track_id = static_cast<uint64_t>(base + i);
      ReviewOutcome o;
      o.confirmed = true;
      o.confidence = "high";
      r->decide(e, QwenOutcome::kSucceeded, o, "");
    }
  };
  std::thread t1(worker, 0);
  std::thread t2(worker, 1000);
  t1.join();
  t2.join();
  CHECK(r->written() == 2 * kPerThread);

  r->close();
  const auto lines = read_lines(path);
  CHECK(lines.size() == 2 * kPerThread);
  std::vector<std::string> ids;
  for (const auto& l : lines) {
    check_required_fields(l);
    ids.push_back(l["event_id"].asString());
  }
  std::sort(ids.begin(), ids.end());
  CHECK(std::adjacent_find(ids.begin(), ids.end()) == ids.end());  // unique
  std::remove(path.c_str());
}

// ---- main -------------------------------------------------------------------

int main() {
  test_parse_valid();
  test_parse_markdown_fence();
  test_parse_invalid_json();
  test_parse_missing_fields_default();
  test_parse_non_bool_confirmed();
  test_parse_unknown_confidence_kept();
  test_classify_table();
  test_decide_success_paths();
  test_decide_failure_and_not_submitted();
  test_decide_disabled_no_file();
  test_decide_stream_idx_out_of_range();
  test_decide_concurrent_integrity();

  std::printf("test_decision_router: %d checks, %d failures\n", g_checks,
              g_failures);
  return g_failures == 0 ? 0 : 1;
}
