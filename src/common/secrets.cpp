// Secrets loader implementation.
//
// Secrets are read from environment variables first; if a variable is not
// set, ~/.jetedge/secrets.env is parsed once per process lifetime as a
// fallback.  Keys are never written to logs or stdout.

#include "jetedge/common/secrets.h"

#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>

#include "jetedge/common/logging.h"

namespace jetedge {
namespace common {

namespace {

constexpr const char* kSecretsPath = "/home/seeed/.jetedge/secrets.env";
constexpr const char* kQwenEnvVar = "QWEN_API_KEY";
constexpr const char* kDeepSeekEnvVar = "DEEPSEEK_API_KEY";

std::once_flag g_load_flag;
int g_load_result = 0;  // 0 = not attempted, 1 = ok, -1 = error

// Trim leading/trailing whitespace (in-place).
void trim(std::string& s) {
  // left
  size_t i = 0;
  while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r')) ++i;
  if (i > 0) s.erase(0, i);
  // right
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
    s.pop_back();
}

// Parse a KEY=value line from the secrets file and call setenv.
// Ignores empty lines and comment lines (first non-whitespace char is '#').
void parse_line(const std::string& line) {
  std::string trimmed = line;
  trim(trimmed);
  if (trimmed.empty() || trimmed[0] == '#') return;

  size_t eq = trimmed.find('=');
  if (eq == std::string::npos || eq == 0) return;  // no '=' or empty key

  std::string key = trimmed.substr(0, eq);
  std::string value = trimmed.substr(eq + 1);
  trim(key);
  trim(value);

  if (key.empty() || value.empty()) return;
  // Don't overwrite an already-set env var — env takes precedence.
  if (getenv(key.c_str()) != nullptr) return;

  setenv(key.c_str(), value.c_str(), /*overwrite=*/0);
}

void do_load() {
  std::ifstream f(kSecretsPath);
  if (!f.is_open()) {
    LOG_WARN("secrets", "cannot open %s", kSecretsPath);
    g_load_result = -1;
    return;
  }

  std::string line;
  int count = 0;
  while (std::getline(f, line)) {
    parse_line(line);
    ++count;
  }
  g_load_result = 1;
  LOG_INFO("secrets", "loaded %d variable(s) from secrets file", count);
}

}  // namespace

int load_secrets_file() {
  std::call_once(g_load_flag, do_load);
  return g_load_result;
}

std::string qwen_api_key() {
  load_secrets_file();
  const char* v = getenv(kQwenEnvVar);
  return v ? std::string(v) : std::string();
}

std::string deepseek_api_key() {
  load_secrets_file();
  const char* v = getenv(kDeepSeekEnvVar);
  return v ? std::string(v) : std::string();
}

}  // namespace common
}  // namespace jetedge
