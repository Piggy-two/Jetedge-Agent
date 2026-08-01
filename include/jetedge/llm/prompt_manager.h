// PromptManager — fixed prompt templates and API request bodies (Stage 7).
//
// Prompt text stays stable (schema-validated, dynamic data placed at the
// end).  Builds OpenAI-compatible chat-completions JSON bodies for Qwen
// (multimodal: text + base64 JPEG) and DeepSeek (text only), with bounded
// output tokens.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "jetedge/events/event_types.h"
#include "jetedge/llm/llm_types.h"

namespace jetedge {
namespace llm {

class PromptManager {
 public:
  // Build the Qwen visual-review prompt from an event.  `has_image` adjusts
  // the instruction ("review the image" vs "review the metadata only").
  std::string build_qwen_prompt(const events::EventRecord& e,
                                const std::vector<std::string>& class_names,
                                bool has_image) const;

  // Build the DeepSeek metrics-diagnosis prompt from the aggregated payload.
  static std::string build_deepseek_metrics_prompt(
      const std::string& metrics_json);

  // System messages (role: "system").
  static std::string qwen_system_message();
  static std::string deepseek_system_message();

  // OpenAI-compatible request body for Qwen: text + base64 image parts.
  // `image_paths` empty → text-only.  Returns "" on encode error.
  // `model` comes from the provider config; `thinking_mode` reserved.
  std::string build_qwen_body(const std::string& model,
                              const std::string& prompt_text,
                              const std::vector<std::string>& image_paths,
                              int max_tokens, bool thinking_mode) const;

  // OpenAI-compatible request body for DeepSeek (text only).
  std::string build_deepseek_body(const std::string& model,
                                  const std::string& prompt_text,
                                  const std::string& system_message,
                                  int max_tokens) const;

  // base64-encode a JPEG file for the data URL.  Returns "" on failure.
  static std::string base64_encode_file(const std::string& path);

  // Parse the "choices[0].message.content" text out of an OpenAI-compatible
  // response body.  Returns false when the shape is unexpected.
  static bool extract_content(const std::string& response_body,
                              std::string& content_out);

  // Strip a surrounding markdown code fence (```json ... ```) and trim
  // whitespace.  LLMs commonly wrap requested JSON in fences; the router
  // stores this normalized form in the cloud-analysis JSONL so records stay
  // directly parseable.
  static std::string strip_markdown_fence(const std::string& content);

  // True when `content` parses as JSON and contains the expected keys.
  // Fences are tolerated (see strip_markdown_fence).
  static bool validate_review_json(const std::string& content);
};

}  // namespace llm
}  // namespace jetedge
