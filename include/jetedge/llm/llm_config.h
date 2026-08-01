// LLM configuration for the Stage 7 async cloud-analysis module.
//
// Parsed from the YAML "llm:" section by the config loader.  Keep defaults
// conservative: cloud analysis is best-effort and must never block or
// terminate the real-time pipeline.

#pragma once

#include <string>

namespace jetedge {
namespace llm {

// One cloud provider endpoint.
struct ProviderEndpoint {
  std::string endpoint;       // full HTTP(S) URL of the chat completions API
  std::string model;          // model name, e.g. "qwen-vl-plus" / "deepseek-chat"
  int max_tokens = 512;       // bounded output tokens
  int timeout_sec = 20;       // HTTP timeout per attempt
  int max_retries = 2;        // retries on transient failures (5xx / timeout)
  bool thinking_mode = false; // Qwen: reasoning mode (only for complex events)
};

// Bounded async queue settings.
struct QueueConfig {
  int max_size = 32;          // 1..256; shed lowest-priority items when full
  int worker_threads = 1;     // 1..8
};

// Circuit breaker per provider.
struct CircuitBreakerConfig {
  int failure_threshold = 5;      // consecutive failures before OPEN
  int reset_timeout_sec = 30;     // OPEN → HALF_OPEN wait
  int half_open_success_threshold = 2;  // consecutive HALF_OPEN successes → CLOSED
};

// Event-type → provider routing table.  Rule-confirmed events stay local;
// only the configured routes are sent to the cloud.
struct RoutingConfig {
  bool appearance_to_qwen = false;    // ambiguous visual review
  bool disappearance_to_qwen = false;
  bool zone_entry_to_qwen = true;     // safety-relevant, visually confirmed
  bool count_high_to_qwen = false;    // crowd event visual confirmation
};

struct LlmConfig {
  bool enable = false;                // master switch (default off)
  std::string keyframe_dir;           // base dir to resolve keyframe paths
  std::string cloud_output_path;      // analysis JSONL (empty = no file)

  ProviderEndpoint qwen;              // visual event review
  ProviderEndpoint deepseek;          // metrics / logs diagnosis

  QueueConfig queue;
  CircuitBreakerConfig circuit_breaker;
  RoutingConfig routing;

  int deepseek_interval_sec = 60;     // periodic system analysis interval
};

}  // namespace llm
}  // namespace jetedge
