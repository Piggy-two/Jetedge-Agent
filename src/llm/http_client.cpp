// HttpClient implementation — see http_client.h.
//
// Uses the libcurl easy API with one reused handle per instance (keep-alive
// connection reuse).  Retries only on transient failures: connection errors
// (CURLE_COULDNT_CONNECT, CURLE_OPERATION_TIMEDOUT, etc.) and HTTP 5xx.
// 4xx responses are not retried (client error; retrying is pointless).

#include "jetedge/llm/http_client.h"

#include <chrono>
#include <cstring>
#include <thread>

#include <curl/curl.h>

#include "jetedge/common/logging.h"

namespace jetedge {
namespace llm {

namespace {

bool is_transient_curl_error(CURLcode code) {
  switch (code) {
    case CURLE_COULDNT_CONNECT:
    case CURLE_OPERATION_TIMEDOUT:
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_RESOLVE_PROXY:
    case CURLE_GOT_NOTHING:
    case CURLE_SEND_ERROR:
    case CURLE_RECV_ERROR:
    case CURLE_HTTP2:
    case CURLE_SSL_CONNECT_ERROR:
    case CURLE_PARTIAL_FILE:
      return true;
    default:
      return false;
  }
}

bool is_transient_http_status(int status) {
  return status >= 500 && status <= 599;
}

}  // namespace

HttpClient::HttpClient() {
  global_inited_ = (curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK);
  if (!global_inited_) {
    LOG_ERROR("llm", "", "http", "HTTP001", "%s", "curl_global_init failed");
  }
  curl_ = curl_easy_init();
  if (!curl_) {
    LOG_ERROR("llm", "", "http", "HTTP002", "%s", "curl_easy_init failed");
  }
}

HttpClient::~HttpClient() {
  if (curl_) {
    curl_easy_cleanup(curl_);
    curl_ = nullptr;
  }
  if (global_inited_) {
    curl_global_cleanup();
  }
}

size_t HttpClient::on_write(char* ptr, size_t size, size_t nmemb, void* user_data) {
  auto* body = static_cast<std::string*>(user_data);
  body->append(ptr, size * nmemb);
  return size * nmemb;
}

HttpClient::HttpResponse HttpClient::post_json(const std::string& url,
                                               const std::string& api_key,
                                               const std::string& json_body,
                                               int timeout_sec, int max_retries) {
  return post_raw(url, api_key, json_body, "application/json", timeout_sec,
                  max_retries);
}

HttpClient::HttpResponse HttpClient::post_raw(const std::string& url,
                                              const std::string& api_key,
                                              const std::string& body,
                                              const std::string& content_type,
                                              int timeout_sec, int max_retries) {
  HttpResponse result;
  if (!curl_) {
    result.error = "curl not initialized";
    return result;
  }

  const auto start = std::chrono::steady_clock::now();
  int backoff_ms = 500;  // doubles each retry: 500, 1000, 2000, ...

  for (int attempt = 0; attempt <= max_retries; ++attempt) {
    std::string response_body;
    std::string error_buf(CURL_ERROR_SIZE, '\0');
    struct curl_slist* headers = nullptr;

    std::unique_lock<std::mutex> lock(mu_);  // serialize handle reuse

    curl_easy_reset(curl_);
    curl_easy_setopt(curl_, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl_, CURLOPT_POST, 1L);
    curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, body.data());
    curl_easy_setopt(curl_, CURLOPT_POSTFIELDSIZE_LARGE,
                     static_cast<curl_off_t>(body.size()));
    curl_easy_setopt(curl_, CURLOPT_TIMEOUT, static_cast<long>(timeout_sec));
    curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, on_write);
    curl_easy_setopt(curl_, CURLOPT_WRITEDATA, &response_body);
    curl_easy_setopt(curl_, CURLOPT_ERRORBUFFER, error_buf.data());
    curl_easy_setopt(curl_, CURLOPT_MAXCONNECTS, 4L);
    // Accept only server certificates validated by the CA store.
    curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYHOST, 2L);

    const std::string ct_header = "Content-Type: " + content_type;
    headers = curl_slist_append(headers, ct_header.c_str());
    const std::string auth = "Authorization: Bearer " + api_key;
    headers = curl_slist_append(headers, auth.c_str());
    headers = curl_slist_append(headers, "Accept: application/json");
    curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, headers);

    CURLcode rc = curl_easy_perform(curl_);
    long http_code = 0;
    curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(headers);
    lock.unlock();

    result.status_code = static_cast<int>(http_code);
    result.body = std::move(response_body);
    result.error.clear();

    if (rc == CURLE_OK && http_code >= 200 && http_code < 300) {
      result.success = true;
      break;
    }

    // Failed.  Fill error info.
    if (rc != CURLE_OK) {
      result.error = std::string(curl_easy_strerror(rc));
    } else {
      result.error = "HTTP " + std::to_string(http_code);
    }

    const bool transient =
        (rc != CURLE_OK && is_transient_curl_error(rc)) ||
        (rc == CURLE_OK && is_transient_http_status(static_cast<int>(http_code)));

    if (!transient || attempt == max_retries) {
      LOG_WARN("llm", "http POST %s failed (attempt %d/%d): %s (curl rc=%d, http=%ld)",
               url.c_str(), attempt + 1, max_retries + 1, result.error.c_str(),
               static_cast<int>(rc), http_code);
      break;
    }

    LOG_WARN("llm", "http POST %s transient failure (attempt %d/%d): %s — retrying in %d ms",
             url.c_str(), attempt + 1, max_retries + 1, result.error.c_str(),
             backoff_ms);
    std::this_thread::sleep_for(std::chrono::milliseconds(backoff_ms));
    backoff_ms *= 2;
  }

  result.latency_ms = static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - start)
          .count());
  return result;
}

}  // namespace llm
}  // namespace jetedge
