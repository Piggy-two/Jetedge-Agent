// JetEdge structured logging (header-only)
//
// Format: timestamp | level | module | stream_id | state | operation | error_code | message
//
// Usage:
//   LOG_INFO("pipeline", "starting pipeline, input=%s", path);
//   LOG_ERROR("pipeline", "", "build", "E001", "failed to create element: %s", name);
//
// Do not log credentials, API keys, or tokens.

#pragma once

#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <string>

namespace jetedge {
namespace logging {

enum class Level { kDebug = 0, kInfo = 1, kWarn = 2, kError = 3 };

inline const char* level_str(Level lv) {
  switch (lv) {
    case Level::kDebug: return "DEBUG";
    case Level::kInfo:  return "INFO";
    case Level::kWarn:  return "WARN";
    case Level::kError: return "ERROR";
  }
  return "???";
}

// Return current local-time timestamp as "HH:MM:SS" (thread-safe via C library).
inline std::string timestamp_now() {
  const std::time_t t = std::time(nullptr);
  std::tm tm_buf{};
  localtime_r(&t, &tm_buf);
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d",
                tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec);
  return buf;
}

inline void emit(Level lv,
                 const char* module,
                 const char* stream_id,
                 const char* state,
                 const char* operation,
                 const char* error_code,
                 const char* fmt, ...) __attribute__((format(printf, 7, 8)));

inline void emit(Level lv,
                 const char* module,
                 const char* stream_id,
                 const char* state,
                 const char* operation,
                 const char* error_code,
                 const char* fmt, ...) {
  std::fprintf(lv >= Level::kError ? stderr : stdout,
               "%s | %s | %s | %s | %s | %s | %s | ",
               timestamp_now().c_str(),
               level_str(lv),
               module ? module : "-",
               stream_id ? stream_id : "-",
               state ? state : "-",
               operation ? operation : "-",
               error_code ? error_code : "-");
  va_list args;
  va_start(args, fmt);
  std::vfprintf(lv >= Level::kError ? stderr : stdout, fmt, args);
  va_end(args);
  std::fputc('\n', lv >= Level::kError ? stderr : stdout);
}

}  // namespace logging
}  // namespace jetedge

// Convenience macros — these are the primary API.
#define LOG_DEBUG(mod, ...) \
  jetedge::logging::emit(jetedge::logging::Level::kDebug, mod, "", "", "", "", __VA_ARGS__)
#define LOG_INFO(mod, ...) \
  jetedge::logging::emit(jetedge::logging::Level::kInfo,  mod, "", "", "", "", __VA_ARGS__)
#define LOG_WARN(mod, ...) \
  jetedge::logging::emit(jetedge::logging::Level::kWarn,  mod, "", "", "", "", __VA_ARGS__)
// ec = error_code, followed by format string and optional format args.
// The "operation" field is always "" for LOG_ERROR.
#define LOG_ERROR(mod, sid, st, ec, ...) \
  jetedge::logging::emit(jetedge::logging::Level::kError, mod, sid, st, "" /* op */, ec, ##__VA_ARGS__)
