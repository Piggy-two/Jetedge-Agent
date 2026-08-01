// LlmRouter — async event routing to Qwen / DeepSeek (Stage 7).
//
// The router owns a bounded priority queue and one or more worker threads
// that make the HTTP calls.  The event probe (streaming thread) only calls
// `enqueue_event()` — mutex-protected queue push, returns immediately.
// Cloud failure never affects the real-time pipeline: events are already
// written locally before the router sees them; the router is best-effort.

#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "jetedge/events/event_types.h"
#include "jetedge/llm/circuit_breaker.h"
#include "jetedge/llm/http_client.h"
#include "jetedge/llm/llm_config.h"
#include "jetedge/llm/llm_types.h"
#include "jetedge/llm/prompt_manager.h"
#include "jetedge/llm/request_queue.h"

namespace jetedge {
namespace llm {

class LlmRouter {
 public:
  LlmRouter() = default;
  ~LlmRouter();

  LlmRouter(const LlmRouter&) = delete;
  LlmRouter& operator=(const LlmRouter&) = delete;

  // Initialize from config.  `stream_ids` maps stream index → id (for the
  // analysis JSONL); `class_names` for prompt building.
  bool init(const LlmConfig& config, const std::vector<std::string>& stream_ids,
            const std::vector<std::string>& class_names);

  // Start the worker thread(s).  No-op when not enabled.
  void start();

  // Stop workers, flush pending state.  Idempotent.
  void stop();

  bool enabled() const { return config_.enable; }

  // Called from the event probe (streaming thread).  Non-blocking.
  // Applies the routing table; high-risk events go to Qwen.  Returns false
  // when the request was shed (queue full) or not routed.
  bool enqueue_event(const events::EventRecord& e, const std::string& keyframe_name);

  // Called from the periodic timer (main loop thread).  Non-blocking.
  // Enqueues a DeepSeek metrics-diagnosis request.
  bool enqueue_metrics_analysis(const std::string& metrics_json);

  struct RouterStats {
    uint64_t enqueued = 0;
    uint64_t shed = 0;
    uint64_t sent = 0;
    uint64_t succeeded = 0;
    uint64_t failed = 0;
    uint64_t skipped_circuit_open = 0;
    size_t queued_now = 0;
  };
  RouterStats stats() const;

 private:
  void worker_loop();
  bool route_event(const events::EventRecord& e, const std::string& keyframe_name,
                   LlmRequest& out_req);
  void process_request(LlmRequest req);
  void write_cloud_result(const LlmRequest& req, const LlmResponse& resp);

  LlmConfig config_;
  std::vector<std::string> stream_ids_;
  std::vector<std::string> class_names_;

  std::unique_ptr<BoundedPriorityQueue<LlmRequest>> queue_;
  std::unique_ptr<HttpClient> http_;
  std::unique_ptr<PromptManager> prompts_;
  std::unique_ptr<CircuitBreaker> cb_qwen_;
  std::unique_ptr<CircuitBreaker> cb_deepseek_;

  std::ofstream analysis_jsonl_;
  mutable std::mutex analysis_mu_;

  std::vector<std::thread> workers_;
  std::atomic<bool> running_{false};
  std::atomic<uint64_t> request_seq_{0};

  mutable std::mutex stats_mu_;
  RouterStats stats_;
};

}  // namespace llm
}  // namespace jetedge
