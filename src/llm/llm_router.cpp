// LlmRouter implementation — see llm_router.h.

#include "jetedge/llm/llm_router.h"

#include <chrono>
#include <cstdio>
#include <ctime>

#include <json/json.h>

#include "jetedge/common/logging.h"
#include "jetedge/common/secrets.h"

namespace jetedge {
namespace llm {

namespace {

uint64_t now_ms() {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000ULL +
         static_cast<uint64_t>(ts.tv_nsec / 1000000);
}

const std::string& stream_id_at(const std::vector<std::string>& ids, int idx) {
  if (idx >= 0 && idx < static_cast<int>(ids.size())) {
    return ids[static_cast<size_t>(idx)];
  }
  static const std::string kUnknown = "unknown";
  return kUnknown;
}

}  // namespace

LlmRouter::~LlmRouter() {
  stop();
}

bool LlmRouter::init(const LlmConfig& config,
                     const std::vector<std::string>& stream_ids,
                     const std::vector<std::string>& class_names) {
  config_ = config;
  stream_ids_ = stream_ids;
  class_names_ = class_names;

  if (!config_.enable) {
    return true;
  }

  // Output file (optional).
  if (!config_.cloud_output_path.empty()) {
    analysis_jsonl_.open(config_.cloud_output_path,
                         std::ios::out | std::ios::app);
    if (!analysis_jsonl_.is_open()) {
      LOG_WARN("llm", "cloud analysis JSONL open failed: %s — results logged only",
               config_.cloud_output_path.c_str());
    } else {
      LOG_INFO("llm", "cloud analysis JSONL: %s", config_.cloud_output_path.c_str());
    }
  }

  // Deterministic post-Qwen decision router (incidents JSONL).
  decision_ = std::make_unique<DecisionRouter>();
  decision_->init(config_.incidents_path, stream_ids);

  queue_ = std::make_unique<BoundedPriorityQueue<LlmRequest>>(
      static_cast<size_t>(config_.queue.max_size > 0 ? config_.queue.max_size : 32));
  http_ = std::make_unique<HttpClient>();
  prompts_ = std::make_unique<PromptManager>();
  cb_qwen_ = std::make_unique<CircuitBreaker>(config_.circuit_breaker, "qwen");
  cb_deepseek_ =
      std::make_unique<CircuitBreaker>(config_.circuit_breaker, "deepseek");

  // Per-provider API keys are resolved at call time (env / secrets file),
  // not copied into the router.
  LOG_INFO("llm", "router initialized: queue=%d worker=%d qwen=%s deepseek=%s",
           config_.queue.max_size, config_.queue.worker_threads,
           config_.qwen.endpoint.empty() ? "off" : config_.qwen.endpoint.c_str(),
           config_.deepseek.endpoint.empty() ? "off"
                                             : config_.deepseek.endpoint.c_str());
  return true;
}

void LlmRouter::start() {
  if (!config_.enable || running_.load()) {
    return;
  }
  running_.store(true);
  const int n = config_.queue.worker_threads > 0 ? config_.queue.worker_threads : 1;
  for (int i = 0; i < n; ++i) {
    workers_.emplace_back(&LlmRouter::worker_loop, this);
  }
  LOG_INFO("llm", "router started with %d worker thread(s)", n);
}

void LlmRouter::stop() {
  if (!running_.exchange(false)) {
    return;
  }
  if (queue_) {
    queue_->shutdown();
  }
  for (auto& t : workers_) {
    if (t.joinable()) {
      t.join();
    }
  }
  workers_.clear();
  if (analysis_jsonl_.is_open()) {
    analysis_jsonl_.flush();
    analysis_jsonl_.close();
  }
  LOG_INFO("llm", "router stopped");
}

bool LlmRouter::route_event(const events::EventRecord& e,
                            const std::string& keyframe_name, LlmRequest& out_req) {
  LlmProvider provider = LlmProvider::kQwen;
  RequestPriority priority = RequestPriority::kNormal;
  bool route = false;

  switch (e.type) {
    case events::EventType::kAppearance:
      route = config_.routing.appearance_to_qwen;
      provider = LlmProvider::kQwen;
      priority = RequestPriority::kNormal;
      break;
    case events::EventType::kDisappearance:
      route = config_.routing.disappearance_to_qwen;
      provider = LlmProvider::kQwen;
      priority = RequestPriority::kNormal;
      break;
    case events::EventType::kZoneEntry:
      route = config_.routing.zone_entry_to_qwen;
      provider = LlmProvider::kQwen;
      priority = RequestPriority::kHigh;  // safety-relevant
      break;
    case events::EventType::kCountHigh:
      route = config_.routing.count_high_to_qwen;
      provider = LlmProvider::kQwen;
      priority = RequestPriority::kHigh;
      break;
    default:
      return false;  // count_exit: rule-confirmed, stays local
  }

  if (!route || config_.qwen.endpoint.empty()) {
    return false;
  }

  out_req.provider = provider;
  out_req.priority = priority;
  out_req.request_id = request_seq_.fetch_add(1);
  out_req.event_type = event_type_str(e.type);
  out_req.stream_idx = e.stream_idx;
  out_req.orig_ts_ms = e.ts_ms;
  out_req.event_copy = e;

  // Build the prompt and the image path list (full paths from keyframe dir).
  std::vector<std::string> image_paths;
  if (!config_.keyframe_dir.empty() && !keyframe_name.empty()) {
    image_paths.push_back(config_.keyframe_dir + "/" + keyframe_name);
  }
  out_req.prompt =
      prompts_->build_qwen_prompt(e, class_names_, !image_paths.empty());
  out_req.image_paths = std::move(image_paths);
  return true;
}

bool LlmRouter::enqueue_event(const events::EventRecord& e,
                              const std::string& keyframe_name) {
  if (!config_.enable) {
    return false;
  }

  LlmRequest req;
  if (!route_event(e, keyframe_name, req)) {
    // Safety-relevant events that never reach the queue still need a
    // deterministic decision: local rule stands (incident recorded).
    if (decision_ && e.type == events::EventType::kZoneEntry) {
      decision_->decide(e, QwenOutcome::kNotSubmitted, {},
                        "not_routed");
    }
    return false;
  }
  const RequestPriority priority = req.priority;
  const std::string event_type = req.event_type;
  const int stream_idx = req.stream_idx;

  {
    std::lock_guard<std::mutex> lock(stats_mu_);
    ++stats_.enqueued;
  }
  if (!queue_ || !queue_->enqueue(std::move(req), priority)) {
    std::lock_guard<std::mutex> lock(stats_mu_);
    ++stats_.shed;
    LOG_WARN("llm", "request queue full — shed %s event (stream %d)",
             event_type.c_str(), stream_idx);
    if (decision_) {
      decision_->decide(e, QwenOutcome::kNotSubmitted, {}, "queue_shed");
    }
    return false;
  }
  return true;
}

bool LlmRouter::enqueue_metrics_analysis(const std::string& metrics_json) {
  if (!config_.enable || config_.deepseek.endpoint.empty()) {
    return false;
  }

  LlmRequest req;
  req.provider = LlmProvider::kDeepSeek;
  req.priority = RequestPriority::kLow;  // periodic, non-urgent
  req.request_id = request_seq_.fetch_add(1);
  req.event_type = "metrics";
  req.stream_idx = -1;
  req.orig_ts_ms = now_ms();
  req.metrics_json = metrics_json;

  {
    std::lock_guard<std::mutex> lock(stats_mu_);
    ++stats_.enqueued;
  }
  if (!queue_ || !queue_->enqueue(std::move(req), req.priority)) {
    std::lock_guard<std::mutex> lock(stats_mu_);
    ++stats_.shed;
    return false;
  }
  return true;
}

void LlmRouter::worker_loop() {
  constexpr std::chrono::milliseconds kDequeueTimeout(100);
  while (running_.load()) {
    auto item = queue_ ? queue_->dequeue(kDequeueTimeout) : std::nullopt;
    if (!item) {
      continue;  // timeout (empty) or shutdown
    }
    process_request(std::move(*item));
  }
}

void LlmRouter::process_request(LlmRequest req) {
  CircuitBreaker* cb =
      (req.provider == LlmProvider::kQwen) ? cb_qwen_.get() : cb_deepseek_.get();

  // Every terminal outcome of a Qwen visual review ends in a deterministic
  // decision (incidents row); DeepSeek metrics rows produce no incidents.
  const auto decide = [this, &req](QwenOutcome outcome,
                                   const ReviewOutcome& review,
                                   const std::string& detail) {
    if (decision_ && req.provider == LlmProvider::kQwen) {
      decision_->decide(req.event_copy, outcome, review, detail);
    }
  };

  // Circuit breaker gate.
  if (!cb->allow_request()) {
    std::lock_guard<std::mutex> lock(stats_mu_);
    ++stats_.skipped_circuit_open;
    LOG_WARN("llm", "circuit open for %s — request %llu skipped",
             llm_provider_str(req.provider),
             static_cast<unsigned long long>(req.request_id));
    decide(QwenOutcome::kNotSubmitted, {}, "circuit_open");
    return;
  }

  // Resolve provider settings + API key.
  const bool is_qwen = (req.provider == LlmProvider::kQwen);
  const ProviderEndpoint& ep = is_qwen ? config_.qwen : config_.deepseek;
  const std::string api_key =
      is_qwen ? common::qwen_api_key() : common::deepseek_api_key();

  if (ep.endpoint.empty()) {
    LOG_WARN("llm", "%s endpoint not configured — request dropped",
             llm_provider_str(req.provider));
    decide(QwenOutcome::kNotSubmitted, {}, "endpoint_empty");
    return;
  }
  if (api_key.empty()) {
    LOG_WARN("llm", "%s API key missing — request dropped (pipeline unaffected)",
             llm_provider_str(req.provider));
    decide(QwenOutcome::kNotSubmitted, {}, "api_key_missing");
    return;
  }

  // Build the request body.
  std::string body;
  if (is_qwen) {
    body = prompts_->build_qwen_body(ep.model, req.prompt, req.image_paths,
                                     ep.max_tokens, ep.thinking_mode);
  } else {
    const std::string prompt =
        req.metrics_json.empty()
            ? req.prompt
            : PromptManager::build_deepseek_metrics_prompt(req.metrics_json);
    body = prompts_->build_deepseek_body(ep.model, prompt,
                                         PromptManager::deepseek_system_message(),
                                         ep.max_tokens, ep.thinking_mode);
  }
  if (body.empty()) {
    LOG_WARN("llm", "%s request body build failed", llm_provider_str(req.provider));
    decide(QwenOutcome::kNotSubmitted, {}, "body_build_failed");
    return;
  }

  {
    std::lock_guard<std::mutex> lock(stats_mu_);
    ++stats_.sent;
  }

  const auto start = std::chrono::steady_clock::now();
  HttpClient::HttpResponse resp =
      http_->post_json(ep.endpoint, api_key, body, ep.timeout_sec, ep.max_retries);
  const uint64_t latency_ms = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - start)
          .count());

  LlmResponse result;
  result.provider = req.provider;
  result.request_id = req.request_id;
  result.event_type = req.event_type;
  result.stream_idx = req.stream_idx;
  result.orig_ts_ms = req.orig_ts_ms;
  result.http_status = resp.status_code;
  result.latency_ms = latency_ms;

  if (resp.success && resp.status_code == 200) {
    std::string content;
    const bool extracted = PromptManager::extract_content(resp.body, content);
    if (extracted && PromptManager::validate_review_json(content)) {
      result.success = true;
      // Store the normalized form so analysis-JSONL records stay directly
      // parseable regardless of model fence habits.
      result.result_json = PromptManager::strip_markdown_fence(content);
      cb->record_success();
      std::lock_guard<std::mutex> lock(stats_mu_);
      ++stats_.succeeded;
      LOG_INFO("llm", "%s ok: %s (req %llu, %llu ms, http %d)",
               llm_provider_str(req.provider), req.event_type.c_str(),
               static_cast<unsigned long long>(req.request_id),
               static_cast<unsigned long long>(latency_ms), resp.status_code);
      ReviewOutcome review;
      if (!DecisionRouter::parse_review(result.result_json, review)) {
        // Should not happen (schema was validated), but fail safe.
        LOG_WARN("llm", "review JSON re-parse failed for req %llu — manual review",
                 static_cast<unsigned long long>(req.request_id));
      }
      decide(QwenOutcome::kSucceeded, review, "");
    } else {
      // Preserve the original error context (CLAUDE.md §10): log whether
      // extraction or schema validation failed and a truncated raw body
      // (model text, no secrets).
      result.error = "schema validation failed: content is not the expected JSON";
      cb->record_failure();
      std::lock_guard<std::mutex> lock(stats_mu_);
      ++stats_.failed;
      LOG_WARN("llm", "%s response schema invalid (extracted=%d): %s | raw=%.240s",
               llm_provider_str(req.provider), extracted ? 1 : 0,
               result.error.c_str(), resp.body.c_str());
      decide(QwenOutcome::kFailed, {}, "schema_invalid");
    }
  } else {
    result.error = resp.error.empty()
                       ? ("HTTP " + std::to_string(resp.status_code))
                       : resp.error;
    cb->record_failure();
    std::lock_guard<std::mutex> lock(stats_mu_);
    ++stats_.failed;
    LOG_ERROR("llm", "", "", "LLM010", "%s call failed: %s (http %d, %llu ms)",
              llm_provider_str(req.provider), result.error.c_str(),
              resp.status_code, static_cast<unsigned long long>(latency_ms));
    decide(QwenOutcome::kFailed, {}, result.error);
  }

  write_cloud_result(req, result);
}

void LlmRouter::write_cloud_result(const LlmRequest& req,
                                   const LlmResponse& resp) {
  if (!analysis_jsonl_.is_open()) {
    return;
  }

  Json::Value rec;
  rec["ts_ms"] = Json::Value::Int64(static_cast<Json::Int64>(now_ms()));
  rec["request_id"] = Json::Value::Int64(static_cast<Json::Int64>(req.request_id));
  rec["orig_ts_ms"] = Json::Value::Int64(static_cast<Json::Int64>(req.orig_ts_ms));
  rec["event_type"] = req.event_type;
  rec["stream_id"] = stream_id_at(stream_ids_, req.stream_idx);
  rec["provider"] = llm_provider_str(resp.provider);
  rec["success"] = resp.success;
  rec["http_status"] = resp.http_status;
  rec["latency_ms"] = Json::Value::Int64(static_cast<Json::Int64>(resp.latency_ms));
  rec["result"] = resp.result_json;  // raw content string (validated JSON)
  rec["error"] = resp.error;

  Json::FastWriter writer;
  const std::string line = writer.write(rec);

  std::lock_guard<std::mutex> lock(analysis_mu_);
  analysis_jsonl_ << line;
  analysis_jsonl_.flush();
}

LlmRouter::RouterStats LlmRouter::stats() const {
  std::lock_guard<std::mutex> lock(stats_mu_);
  RouterStats s = stats_;
  if (queue_) {
    s.queued_now = queue_->size();
  }
  return s;
}

}  // namespace llm
}  // namespace jetedge
