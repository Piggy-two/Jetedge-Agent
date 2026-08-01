// HttpClient — libcurl POST wrapper for LLM chat completions (Stage 7).
//
// One instance per provider; the curl easy handle is reused across calls so
// connections and TLS sessions are kept alive (CURLOPT_MAXCONNECTS).  All
// calls are synchronous and are made from the LLM worker thread — never
// from a GStreamer streaming thread.

#pragma once

#include <cstdint>
#include <mutex>
#include <string>

namespace jetedge {
namespace llm {

class HttpClient {
 public:
  HttpClient();
  ~HttpClient();

  HttpClient(const HttpClient&) = delete;
  HttpClient& operator=(const HttpClient&) = delete;

  struct HttpResponse {
    bool success = false;        // transport ok (HTTP status received)
    int status_code = 0;         // 0 when transport failed
    std::string body;
    std::string error;           // curl error string
    uint64_t latency_ms = 0;     // whole call duration (incl. retries)
  };

  // POST a JSON body with a Bearer token.  Retries on transient failures
  // (connect errors, timeouts, 5xx) with exponential backoff, up to
  // `max_retries` retries.  Returns the final response.
  HttpResponse post_json(const std::string& url, const std::string& api_key,
                         const std::string& json_body, int timeout_sec,
                         int max_retries);

  // Post `body` raw (used by PromptManager tests / future non-JSON bodies).
  HttpResponse post_raw(const std::string& url, const std::string& api_key,
                        const std::string& body, const std::string& content_type,
                        int timeout_sec, int max_retries);

 private:
  static size_t on_write(char* ptr, size_t size, size_t nmemb, void* user_data);

  void* curl_ = nullptr;         // CURL* (libcurl), reused for connection reuse
  std::mutex mu_;                // serializes calls on the shared handle
  bool global_inited_ = false;
};

}  // namespace llm
}  // namespace jetedge
