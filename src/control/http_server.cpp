#include "jetedge/control/http_server.h"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>

#include "jetedge/common/logging.h"

namespace jetedge {
namespace control {

namespace {

constexpr size_t kMaxHeadBytes = 16 * 1024;  // request line + headers cap

const char* status_text(int status) {
  switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Payload Too Large";
    case 500: return "Internal Server Error";
    default:  return "Unknown";
  }
}

// Minimal JSON error response; always closes the connection.
void send_error(int fd, int status, const char* error_code, bool cors) {
  const std::string body = std::string("{\"error_code\":\"") + error_code + "\"}";
  std::string resp =
      "HTTP/1.1 " + std::to_string(status) + " " + status_text(status) + "\r\n"
      "Content-Type: application/json\r\n"
      "Content-Length: " + std::to_string(body.size()) + "\r\n";
  if (cors) {
    resp += "Access-Control-Allow-Origin: *\r\n";
  }
  resp += "Connection: close\r\n\r\n" + body;
  const ssize_t n = ::write(fd, resp.c_str(), resp.size());
  (void)n;  // connection is closed regardless; EPIPE is expected on reset
}

}  // namespace

HttpServer::~HttpServer() {
  stop();
}

bool HttpServer::start(const std::string& host, int port, Handler handler,
                       size_t max_body_bytes, int read_timeout_ms, bool cors) {
  if (running_) {
    return false;
  }
  handler_ = std::move(handler);
  max_body_bytes_ = max_body_bytes;
  read_timeout_ms_ = read_timeout_ms;
  cors_ = cors;

  listen_fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd_ < 0) {
    LOG_ERROR("control", "", "start", "HTTP010", "socket() failed: %s",
              std::strerror(errno));
    return false;
  }
  int yes = 1;
  ::setsockopt(listen_fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

  struct sockaddr_in addr;
  std::memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port));
  if (host == "0.0.0.0" || host.empty()) {
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
  } else if (::inet_pton(AF_INET, host.c_str(), &addr.sin_addr) != 1) {
    LOG_ERROR("control", "", "start", "HTTP011", "invalid host '%s'", host.c_str());
    ::close(listen_fd_);
    listen_fd_ = -1;
    return false;
  }

  if (::bind(listen_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
    LOG_ERROR("control", "", "start", "HTTP012", "bind %s:%d failed: %s",
              host.c_str(), port, std::strerror(errno));
    ::close(listen_fd_);
    listen_fd_ = -1;
    return false;
  }
  if (::listen(listen_fd_, 16) < 0) {
    LOG_ERROR("control", "", "start", "HTTP013", "listen failed: %s",
              std::strerror(errno));
    ::close(listen_fd_);
    listen_fd_ = -1;
    return false;
  }

  // Read the actual bound port (supports port 0 in tests).
  struct sockaddr_in bound;
  socklen_t bound_len = sizeof(bound);
  if (::getsockname(listen_fd_, reinterpret_cast<struct sockaddr*>(&bound),
                    &bound_len) == 0) {
    port_ = ntohs(bound.sin_port);
  } else {
    port_ = port;
  }

  running_ = true;
  accept_thread_ = std::thread(&HttpServer::accept_loop, this);
  LOG_INFO("control", "control API listening on %s:%d", host.c_str(), port_);
  return true;
}

void HttpServer::stop() {
  if (!running_) {
    return;
  }
  running_ = false;
  // Wake a blocked accept().
  ::shutdown(listen_fd_, SHUT_RDWR);
  if (accept_thread_.joinable()) {
    accept_thread_.join();
  }
  if (listen_fd_ >= 0) {
    ::close(listen_fd_);
    listen_fd_ = -1;
  }
}

void HttpServer::accept_loop() {
  while (running_) {
    struct sockaddr_in peer;
    socklen_t peer_len = sizeof(peer);
    const int cfd = ::accept(listen_fd_, reinterpret_cast<struct sockaddr*>(&peer),
                             &peer_len);
    if (cfd < 0) {
      if (errno == EINTR || errno == EAGAIN) {
        continue;
      }
      if (!running_) {
        break;  // listener shut down
      }
      LOG_WARN("control", "accept() failed: %s", std::strerror(errno));
      continue;
    }
    handle_connection(cfd);
  }
}

void HttpServer::handle_connection(int fd) {
  // Read timeout bounds the time one slow client can block the accept loop.
  struct timeval tv;
  tv.tv_sec = read_timeout_ms_ / 1000;
  tv.tv_usec = (read_timeout_ms_ % 1000) * 1000;
  ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  std::string buffer;
  buffer.reserve(kMaxHeadBytes);
  size_t head_end = std::string::npos;

  char chunk[4096];
  while (head_end == std::string::npos) {
    if (buffer.size() > kMaxHeadBytes) {
      // Header section too large.
      send_error(fd, 400, "HTTP_TOO_LARGE", cors_);
      ::close(fd);
      return;
    }
    const ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
    if (n <= 0) {
      ::close(fd);
      return;
    }
    buffer.append(chunk, static_cast<size_t>(n));
    head_end = buffer.find("\r\n\r\n");
  }

  // Read the body up to Content-Length (bounded by max_body_bytes_).
  const std::string head = buffer.substr(0, head_end);
  std::string body = buffer.substr(head_end + 4);
  int64_t content_length = 0;
  {
    // Validate the request line + headers and read Content-Length BEFORE
    // draining the body (parse_request would reject a truncated body).
    const size_t eol = head.find("\r\n");
    if (eol == std::string::npos) {
      send_error(fd, 400, "HTTP_PARSE", cors_);
      ::close(fd);
      return;
    }
    std::string method;
    std::string path;
    std::string query;
    if (!parse_request_line(head.substr(0, eol), &method, &path, &query)) {
      send_error(fd, 400, "HTTP_PARSE", cors_);
      ::close(fd);
      return;
    }
    if (method == "OPTIONS") {
      // CORS preflight (Stage 15): no body, no handler call.  Only answered
      // when CORS is enabled; a disabled server must not accept OPTIONS.
      if (!cors_) {
        send_error(fd, 405, "HTTP_METHOD", cors_);
        ::close(fd);
        return;
      }
      const std::string preflight =
          "HTTP/1.1 200 OK\r\n"
          "Access-Control-Allow-Origin: *\r\n"
          "Access-Control-Allow-Methods: GET, POST, OPTIONS\r\n"
          "Access-Control-Allow-Headers: Content-Type\r\n"
          "Access-Control-Max-Age: 3600\r\n"
          "Content-Length: 0\r\n"
          "Connection: close\r\n\r\n";
      const ssize_t n = ::write(fd, preflight.c_str(), preflight.size());
      (void)n;
      ::close(fd);
      return;
    }
    if (method != "GET" && method != "POST") {
      send_error(fd, 405, "HTTP_METHOD", cors_);
      ::close(fd);
      return;
    }
    std::map<std::string, std::string> headers;
    if (!parse_headers(head.substr(eol + 2), &headers)) {
      send_error(fd, 400, "HTTP_PARSE", cors_);
      ::close(fd);
      return;
    }
    content_length = parse_content_length(headers);
    if (content_length < 0 ||
        static_cast<uint64_t>(content_length) > max_body_bytes_) {
      send_error(fd, 413, "HTTP_TOO_LARGE", cors_);
      ::close(fd);
      return;
    }
  }

  while (body.size() < static_cast<size_t>(content_length)) {
    const ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
    if (n <= 0) {
      break;  // client closed early — parse what we have
    }
    body.append(chunk, static_cast<size_t>(n));
  }

  HttpRequest req;
  HttpResponse resp;
  if (parse_request(head, body, max_body_bytes_, &req) && handler_) {
    try {
      resp = handler_(req);
    } catch (...) {
      resp = HttpResponse{500, "application/json", "{\"error_code\":\"INTERNAL\"}"};
    }
  } else {
    resp = HttpResponse{400, "application/json", "{\"error_code\":\"HTTP_PARSE\"}"};
  }

  std::string response =
      "HTTP/1.1 " + std::to_string(resp.status) + " " + status_text(resp.status) + "\r\n"
      "Content-Type: " + resp.content_type + "\r\n"
      "Content-Length: " + std::to_string(resp.body.size()) + "\r\n";
  if (cors_) {
    response += "Access-Control-Allow-Origin: *\r\n";
  }
  response += "Connection: close\r\n\r\n" + resp.body;
  const ssize_t n = ::write(fd, response.c_str(), response.size());
  (void)n;  // connection is closed regardless; EPIPE is expected on reset
  ::close(fd);
}

// ---- Pure parsing helpers ---------------------------------------------------

bool HttpServer::parse_request_line(const std::string& line, std::string* method,
                                    std::string* path, std::string* query) {
  const size_t sp1 = line.find(' ');
  if (sp1 == std::string::npos) {
    return false;
  }
  const size_t sp2 = line.find(' ', sp1 + 1);
  if (sp2 == std::string::npos) {
    return false;
  }
  const std::string version = line.substr(sp2 + 1);
  if (version != "HTTP/1.1" && version != "HTTP/1.0") {
    return false;
  }
  *method = line.substr(0, sp1);
  if (method->empty()) {
    return false;
  }

  std::string target = line.substr(sp1 + 1, sp2 - sp1 - 1);
  const size_t qpos = target.find('?');
  if (qpos != std::string::npos) {
    *query = target.substr(qpos + 1);
    target = target.substr(0, qpos);
  } else {
    query->clear();
  }
  if (target.empty() || target[0] != '/') {
    return false;
  }
  std::string decoded;
  if (!url_decode(target, &decoded)) {
    return false;
  }
  *path = std::move(decoded);
  return true;
}

bool HttpServer::parse_headers(const std::string& block,
                               std::map<std::string, std::string>* out) {
  out->clear();
  size_t pos = 0;
  while (pos < block.size()) {
    const size_t eol = block.find("\r\n", pos);
    const size_t line_end = eol == std::string::npos ? block.size() : eol;
    const std::string line = block.substr(pos, line_end - pos);
    if (line.empty()) {
      // Tolerate a trailing blank line: callers may include the
      // CRLFCRLF terminator in the header block.
      if (eol == std::string::npos) {
        break;
      }
      pos = eol + 2;
      continue;
    }
    const size_t colon = line.find(':');
    if (colon == std::string::npos) {
      return false;
    }
    std::string name = line.substr(0, colon);
    size_t value_start = colon + 1;
    while (value_start < line.size() && line[value_start] == ' ') {
      ++value_start;
    }
    // Lowercase the name.
    for (char& c : name) {
      if (c >= 'A' && c <= 'Z') {
        c += 'a' - 'A';
      }
    }
    (*out)[name] = line.substr(value_start);
    if (eol == std::string::npos) {
      break;
    }
    pos = eol + 2;
  }
  return true;
}

int64_t HttpServer::parse_content_length(
    const std::map<std::string, std::string>& headers) {
  const auto it = headers.find("content-length");
  if (it == headers.end()) {
    return 0;
  }
  int64_t v = 0;
  const std::string& s = it->second;
  if (s.empty()) {
    return -1;
  }
  for (const char c : s) {
    if (c < '0' || c > '9') {
      return -1;
    }
    v = v * 10 + (c - '0');
    if (v < 0) {
      return -1;  // overflow
    }
  }
  return v;
}

bool HttpServer::url_decode(const std::string& in, std::string* out) {
  std::string decoded;
  decoded.reserve(in.size());
  for (size_t i = 0; i < in.size(); ++i) {
    if (in[i] == '%') {
      if (i + 2 >= in.size()) {
        return false;
      }
      const auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
      };
      const int hi = hex(in[i + 1]);
      const int lo = hex(in[i + 2]);
      if (hi < 0 || lo < 0) {
        return false;
      }
      decoded.push_back(static_cast<char>((hi << 4) | lo));
      i += 2;
    } else if (in[i] == '+') {
      decoded.push_back(' ');
    } else {
      decoded.push_back(in[i]);
    }
  }
  *out = std::move(decoded);
  return true;
}

bool HttpServer::parse_request(const std::string& head, const std::string& body,
                               size_t max_body_bytes, HttpRequest* out) {
  const size_t eol = head.find("\r\n");
  if (eol == std::string::npos) {
    return false;
  }
  HttpRequest req;
  if (!parse_request_line(head.substr(0, eol), &req.method, &req.path, &req.query)) {
    return false;
  }
  if (req.method != "GET" && req.method != "POST") {
    return false;
  }

  std::map<std::string, std::string> headers;
  if (!parse_headers(head.substr(eol + 2), &headers)) {
    return false;
  }
  const int64_t cl = parse_content_length(headers);
  if (cl < 0 || static_cast<uint64_t>(cl) > max_body_bytes) {
    return false;
  }
  if (body.size() < static_cast<size_t>(cl)) {
    return false;  // body truncated
  }
  req.body = body.substr(0, static_cast<size_t>(cl));
  *out = std::move(req);
  return true;
}

}  // namespace control
}  // namespace jetedge
