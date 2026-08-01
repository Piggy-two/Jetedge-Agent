// Secrets loader — reads API keys from environment variables or
// ~/.jetedge/secrets.env.  Never logs, prints, or exposes key values.
//
// Precedence: environment variable > secrets file.  An empty string is
// returned when no key is configured; the caller must decide whether a
// missing key is fatal or just disables that provider.

#pragma once

#include <string>

namespace jetedge {
namespace common {

// Load secrets from ~/.jetedge/secrets.env into the process environment.
// Safe to call multiple times — parses the file once per process lifetime.
// Returns the number of variables loaded, or -1 on file-open error.
int load_secrets_file();

// Per-provider accessors.  Returns "" when not configured.
std::string qwen_api_key();
std::string deepseek_api_key();

}  // namespace common
}  // namespace jetedge
