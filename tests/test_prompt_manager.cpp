// Unit tests for the Stage 7 PromptManager response parsing.
//
// Exercises: extract_content on OpenAI-compatible bodies; validate_review_json
// on plain JSON, markdown-fenced JSON (```json / bare ```), whitespace-padded
// content, wrong-schema JSON, and non-JSON prose.  Fence stripping was added
// after live DashScope responses (qwen-vl-plus, qwen3.6-flash) wrapped the
// requested JSON in ```json fences (2026-08-01).

#include <cstdio>
#include <string>

#include "jetedge/llm/prompt_manager.h"

using jetedge::llm::PromptManager;

namespace {

int g_failures = 0;

void check(bool cond, const char* what) {
  if (cond) {
    std::printf("  PASS  %s\n", what);
  } else {
    std::printf("  FAIL  %s\n", what);
    ++g_failures;
  }
}

}  // namespace

int main() {
  // ---- 1. extract_content: OpenAI-compatible response bodies ----
  {
    std::string out;
    const std::string body =
        "{\"choices\":[{\"message\":{\"role\":\"assistant\","
        "\"content\":\"{\\\"confirmed\\\":true}\"}}]}";
    check(PromptManager::extract_content(body, out),
          "1a: valid body extracts content");
    check(out == "{\"confirmed\":true}", "1b: extracted text is the content");
    check(!PromptManager::extract_content("not json", out),
          "1c: non-JSON body rejected");
    check(!PromptManager::extract_content("{\"choices\":[]}", out),
          "1d: empty choices rejected");
    check(!PromptManager::extract_content(
              "{\"choices\":[{\"message\":{\"content\":[{\"type\":\"text\"}]}}]}",
              out),
          "1e: non-string content rejected");
  }

  // ---- 2. validate_review_json: plain JSON ----
  {
    check(PromptManager::validate_review_json(
              "{\"confirmed\": true, \"summary\": \"ok\", "
              "\"confidence\": \"high\"}"),
          "2a: qwen shape plain JSON accepted");
    check(PromptManager::validate_review_json(
              "{\"healthy\": true, \"issues\": [], "
              "\"recommendations\": [\"none\"]}"),
          "2b: deepseek shape plain JSON accepted");
  }

  // ---- 3. validate_review_json: markdown code fences ----
  {
    check(PromptManager::validate_review_json(
              "```json\n{\"confirmed\": true, \"summary\": \"ok\", "
              "\"confidence\": \"high\"}\n```"),
          "3a: ```json fence accepted");
    check(PromptManager::validate_review_json(
              "```\n{\"healthy\": true, \"issues\": [], "
              "\"recommendations\": [\"none\"]}\n```"),
          "3b: bare ``` fence accepted");
    check(PromptManager::validate_review_json(
              "  \n```json\n{\"confirmed\": false, \"summary\": \"n\", "
              "\"confidence\": \"low\"}\n```\n  "),
          "3c: fence with surrounding whitespace accepted");
    check(!PromptManager::validate_review_json("```json\n```"),
          "3d: fence-only content rejected");
  }

  // ---- 4. validate_review_json: rejected shapes ----
  {
    check(!PromptManager::validate_review_json("this is prose, not JSON"),
          "4a: prose rejected");
    check(!PromptManager::validate_review_json(
              "{\"confirmed\": true}"),
          "4b: missing required keys rejected");
    check(!PromptManager::validate_review_json(
              "{\"confirmed\": \"yes\", \"summary\": \"s\"}"),
          "4c: wrong field types rejected");
    check(!PromptManager::validate_review_json(""),
          "4d: empty content rejected");
    check(!PromptManager::validate_review_json(
              "```json\nthis is prose inside a fence\n```"),
          "4e: fenced prose rejected");
  }

  // ---- 5. strip_markdown_fence: normalization for the analysis JSONL ----
  {
    check(PromptManager::strip_markdown_fence(
              "```json\n{\"confirmed\": true}\n```") == "{\"confirmed\": true}",
          "5a: ```json fence stripped");
    check(PromptManager::strip_markdown_fence(
              "{\"confirmed\": true}") == "{\"confirmed\": true}",
          "5b: plain JSON passed through unchanged");
    check(PromptManager::strip_markdown_fence(
              "  \n```\n{\"healthy\": true}\n```  ") == "{\"healthy\": true}",
          "5c: bare fence with whitespace stripped");
    check(PromptManager::strip_markdown_fence("```json\n```").empty(),
          "5d: fence-only content empty");
  }

  std::printf("\n%s (%d failure(s))\n", g_failures == 0 ? "ALL PASS" : "FAILURES",
              g_failures);
  return g_failures == 0 ? 0 : 1;
}
