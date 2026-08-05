// HttpServer — minimal HTTP/1.1 server for the Control API (Stage 11).
//
// The Jetson has no installed HTTP server library and system packages must
// not be installed, so this is a self-contained POSIX-socket implementation:
//
//   - single accept thread, one connection handled inline (read timeout
//     bounds the worst-case blocking)
//   - request head capped (16 KiB), body capped (max_body_bytes)
//   - Connection: close (no keep-alive — control API is low-rate)
//   - the parse functions are pure and unit-testable without sockets
//
// The server is deliberately outside the real-time path: a slow or broken
// client can only delay its own connection, never the pipeline.

#pragma once

#include <cstdint>
#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <thread>

namespace jetedge {
namespace control {

struct HttpRequest {
  std::string method;   // "GET" | "POST" | "OPTIONS"
  std::string path;     // decoded, e.g. "/streams/cam1/infer-interval"
  std::string query;    // raw query string (after '?'), empty when none
  std::string body;
};

struct HttpResponse {
  int status = 200;
  std::string content_type = "application/json";
  std::string body;
};

class HttpServer {
 public:
  using Handler = std::function<HttpResponse(const HttpRequest&)>;

  HttpServer() = default;
  ~HttpServer();

  HttpServer(const HttpServer&) = delete;
  HttpServer& operator=(const HttpServer&) = delete;

  // Bind + listen + spawn the accept thread.  Returns false when the port
  // cannot be bound.  `handler` must be thread-safe (called from the accept
  // thread only, so single-threaded in practice).
  //
  // `cors` (Stage 15): when true, every response carries
  // "Access-Control-Allow-Origin: *" and OPTIONS preflight requests are
  // answered with the CORS headers.  Default false — the API surface stays
  // minimal unless the web dashboard is explicitly enabled.
  bool start(const std::string& host, int port, Handler handler,
             size_t max_body_bytes = 4096, int read_timeout_ms = 5000,
             bool cors = false);

  // Stop accepting, close the listener, join the accept thread.  Idempotent.
  void stop();

  bool running() const { return running_; }
  int port() const { return port_; }

  // ---- Pure parsing helpers (unit-testable) --------------------------------

  // "METHOD SP PATH[?QUERY] SP HTTP/1.1" → false on malformed request line.
  static bool parse_request_line(const std::string& line, std::string* method,
                                 std::string* path, std::string* query);

  // "Name: value\r\n..." (without the final blank line) → lowercase-key map.
  static bool parse_headers(const std::string& block,
                            std::map<std::string, std::string>* out);

  // Content-Length value; 0 when absent; -1 when malformed/negative.
  static int64_t parse_content_length(const std::map<std::string, std::string>& headers);

  // Percent-decoding ("+" → space).  False on malformed %XX.
  static bool url_decode(const std::string& in, std::string* out);

  // Full request: `head` is everything up to and including the blank line.
  // Returns false on a parse error or when Content-Length exceeds max_body.
  static bool parse_request(const std::string& head, const std::string& body,
                            size_t max_body_bytes, HttpRequest* out);

 private:
  void accept_loop();
  void handle_connection(int fd);

  int listen_fd_ = -1;
  int port_ = 0;
  volatile bool running_ = false;
  std::thread accept_thread_;
  Handler handler_;
  size_t max_body_bytes_ = 4096;
  int read_timeout_ms_ = 5000;
  bool cors_ = false;
};

}  // namespace control
}  // namespace jetedge
