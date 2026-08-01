// PromptManager implementation — see prompt_manager.h.

#include "jetedge/llm/prompt_manager.h"

#include <cstdio>
#include <fstream>
#include <sstream>

#include <json/json.h>

#include "jetedge/common/logging.h"

namespace jetedge {
namespace llm {

namespace {

// Base64 alphabet.
constexpr const char kBase64Chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const unsigned char* data, size_t len) {
  std::string out;
  out.reserve(((len + 2) / 3) * 4);
  for (size_t i = 0; i < len; i += 3) {
    const unsigned int a = data[i];
    const unsigned int b = (i + 1 < len) ? data[i + 1] : 0;
    const unsigned int c = (i + 2 < len) ? data[i + 2] : 0;
    out.push_back(kBase64Chars[(a >> 2) & 0x3F]);
    out.push_back(kBase64Chars[((a << 4) | (b >> 4)) & 0x3F]);
    out.push_back((i + 1 < len) ? kBase64Chars[((b << 2) | (c >> 6)) & 0x3F] : '=');
    out.push_back((i + 2 < len) ? kBase64Chars[c & 0x3F] : '=');
  }
  return out;
}

// Read a whole file into a string (for the JPEG payload).
bool read_file(const std::string& path, std::string& out) {
  std::ifstream f(path, std::ios::binary);
  if (!f.is_open()) {
    return false;
  }
  std::ostringstream ss;
  ss << f.rdbuf();
  out = ss.str();
  return !out.empty();
}

std::string trim(const std::string& s) {
  const size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) {
    return std::string();
  }
  const size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

}  // namespace

// Strip a surrounding markdown code fence (```json ... ```) from model
// output.  LLMs commonly wrap requested JSON in fences even when told to
// respond with "ONLY" JSON — observed live on DashScope qwen-vl-plus and
// qwen3.6-flash (2026-08-01); jsoncpp rejects the backtick lines.  Returns
// "" when nothing but a fence is present.
std::string PromptManager::strip_markdown_fence(const std::string& content) {
  std::string s = trim(content);
  if (s.size() >= 3 && s.compare(0, 3, "```") == 0) {
    const size_t nl = s.find('\n');
    if (nl == std::string::npos) {
      return std::string();  // fence only, no payload
    }
    s = trim(s.substr(nl + 1));
  }
  if (s.size() >= 3 && s.compare(s.size() - 3, 3, "```") == 0) {
    const size_t cut = s.rfind('\n');
    if (cut == std::string::npos) {
      return std::string();  // fence only, no payload
    }
    s = trim(s.substr(0, cut));
  }
  return s;
}

std::string PromptManager::build_qwen_prompt(
    const events::EventRecord& e, const std::vector<std::string>& class_names,
    bool has_image) const {
  std::string class_name = "?";
  if (e.class_id >= 0 && e.class_id < static_cast<int>(class_names.size())) {
    class_name = class_names[static_cast<size_t>(e.class_id)];
  }

  char buf[512];
  std::snprintf(buf, sizeof(buf),
                "Event: %s, class=%s (id %d), confidence=%.2f, "
                "track=%llu, bbox=[%.1f, %.1f, %.1f, %.1f]",
                event_type_str(e.type), class_name.c_str(), e.class_id,
                e.confidence, static_cast<unsigned long long>(e.track_id),
                e.left, e.top, e.width, e.height);

  std::string prompt;
  if (has_image) {
    prompt = "Review the attached image and this detection event. ";
  } else {
    prompt = "No image is available. Review this detection event from its "
             "metadata only. ";
  }
  prompt += "Decide whether the detection is accurate. Respond with ONLY a "
            "JSON object: {\"confirmed\": true|false, \"summary\": \"one "
            "sentence\", \"confidence\": \"high|medium|low\"}. ";
  prompt += buf;
  return prompt;
}

std::string PromptManager::build_deepseek_metrics_prompt(
    const std::string& metrics_json) {
  return "Analyze this edge AI system metrics snapshot. Identify any "
         "performance degradation, resource pressure, or anomalies, and "
         "recommend bounded corrective actions. Respond with ONLY a JSON "
         "object: {\"healthy\": true|false, \"issues\": [\"...\"], "
         "\"recommendations\": [\"...\"]}. Metrics:\n" +
         metrics_json;
}

std::string PromptManager::qwen_system_message() {
  return "You are a traffic-monitoring visual analyst for an edge AI "
         "platform. You review detection events and keyframes. You always "
         "respond with valid JSON only, matching the requested schema. You "
         "never fabricate objects that are not visible in the image.";
}

std::string PromptManager::deepseek_system_message() {
  return "You are a system operator for an edge AI platform. You analyze "
         "metrics, logs, and events. You always respond with valid JSON only, "
         "matching the requested schema. Recommendations must be conservative "
         "and bounded; never suggest disabling all streams.";
}

std::string PromptManager::build_qwen_body(
    const std::string& model, const std::string& prompt_text,
    const std::vector<std::string>& image_paths, int max_tokens,
    bool thinking_mode) const {
  (void)thinking_mode;  // non-thinking default; reasoning added via config later

  Json::Value root;
  root["model"] = model.empty() ? "qwen3.6-flash" : model;
  root["max_tokens"] = max_tokens > 0 ? max_tokens : 256;

  Json::Value messages(Json::arrayValue);
  Json::Value sys_msg;
  sys_msg["role"] = "system";
  sys_msg["content"] = qwen_system_message();
  messages.append(sys_msg);

  Json::Value user_msg;
  user_msg["role"] = "user";
  Json::Value content(Json::arrayValue);

  Json::Value text_part;
  text_part["type"] = "text";
  text_part["text"] = prompt_text;
  content.append(text_part);

  for (const auto& path : image_paths) {
    const std::string b64 = base64_encode_file(path);
    if (b64.empty()) {
      LOG_WARN("llm", "failed to encode image for Qwen: %s", path.c_str());
      continue;
    }
    Json::Value img_part;
    img_part["type"] = "image_url";
    img_part["image_url"]["url"] = "data:image/jpeg;base64," + b64;
    content.append(img_part);
  }

  user_msg["content"] = content;
  messages.append(user_msg);
  root["messages"] = messages;

  Json::FastWriter writer;
  return writer.write(root);
}

std::string PromptManager::build_deepseek_body(const std::string& model,
                                               const std::string& prompt_text,
                                               const std::string& system_message,
                                               int max_tokens) const {
  Json::Value root;
  root["model"] = model.empty() ? "deepseek-chat" : model;
  root["max_tokens"] = max_tokens > 0 ? max_tokens : 512;
  root["stream"] = false;

  Json::Value messages(Json::arrayValue);
  Json::Value sys_msg;
  sys_msg["role"] = "system";
  sys_msg["content"] = system_message.empty() ? deepseek_system_message()
                                              : system_message;
  messages.append(sys_msg);

  Json::Value user_msg;
  user_msg["role"] = "user";
  user_msg["content"] = prompt_text;
  messages.append(user_msg);

  root["messages"] = messages;

  Json::FastWriter writer;
  return writer.write(root);
}

std::string PromptManager::base64_encode_file(const std::string& path) {
  std::string data;
  if (!read_file(path, data)) {
    return "";
  }
  return base64_encode(
      reinterpret_cast<const unsigned char*>(data.data()), data.size());
}

bool PromptManager::extract_content(const std::string& response_body,
                                    std::string& content_out) {
  Json::Value root;
  Json::Reader reader;
  if (!reader.parse(response_body, root) || !root.isObject()) {
    return false;
  }
  if (!root["choices"].isArray() || root["choices"].empty()) {
    return false;
  }
  const Json::Value& choice = root["choices"][0];
  if (!choice["message"]["content"].isString()) {
    return false;
  }
  content_out = choice["message"]["content"].asString();
  return true;
}

bool PromptManager::validate_review_json(const std::string& content) {
  const std::string json_text = strip_markdown_fence(content);
  Json::Value root;
  Json::Reader reader;
  if (!reader.parse(json_text, root) || !root.isObject()) {
    return false;
  }
  // Accept both Qwen ("confirmed" + "summary" + "confidence") and
  // DeepSeek ("healthy" + "issues" + "recommendations") shapes.
  if (root.isMember("confirmed") && root["confirmed"].isBool() &&
      root.isMember("summary") && root["summary"].isString()) {
    return true;
  }
  if (root.isMember("healthy") && root["healthy"].isBool() &&
      root.isMember("issues") && root["issues"].isArray()) {
    return true;
  }
  return false;
}

}  // namespace llm
}  // namespace jetedge
