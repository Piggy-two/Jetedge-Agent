// EventWriter implementation.

#include "jetedge/events/event_writer.h"

#include <cstdio>

#include "jetedge/common/logging.h"

namespace jetedge {
namespace events {

bool EventWriter::open(const std::string& path,
                       const std::vector<std::string>& stream_ids,
                       const std::vector<std::string>& class_names) {
  std::lock_guard<std::mutex> lock(mu_);
  stream_ids_ = stream_ids;
  class_names_ = class_names;
  written_ = 0;
  if (!path.empty()) {
    jsonl_.open(path, std::ios::out | std::ios::trunc);
    if (!jsonl_.is_open()) {
      LOG_ERROR("events", "", "open", "EVT001", "cannot open event JSONL %s",
                path.c_str());
      return false;
    }
    LOG_INFO("events", "event JSONL output: %s", path.c_str());
  }
  return true;
}

void EventWriter::write(const EventRecord& e, const std::string& keyframe_path) {
  std::lock_guard<std::mutex> lock(mu_);

  std::string stream_id = "unknown";
  if (e.stream_idx >= 0 &&
      e.stream_idx < static_cast<int>(stream_ids_.size())) {
    stream_id = stream_ids_[static_cast<size_t>(e.stream_idx)];
  }
  const char* cls = "?";
  if (e.class_id >= 0 && e.class_id < static_cast<int>(class_names_.size())) {
    cls = class_names_[static_cast<size_t>(e.class_id)].c_str();
  }

  char line[640];
  const bool zone_event = (e.type == EventType::kZoneEntry);
  const bool bbox_event =
      (e.type == EventType::kAppearance || e.type == EventType::kDisappearance ||
       e.type == EventType::kZoneEntry);

  // keyframe file names and zone names are sanitized/configured (alnum and
  // '-','_','.'), so no JSON escaping beyond quotes is needed.
  const std::string keyframe_json =
      keyframe_path.empty()
          ? std::string("null")
          : "\"" + keyframe_path + "\"";
  const std::string zone_json =
      zone_event ? "\"" + e.zone + "\"" : std::string("null");

  int n = 0;
  if (bbox_event) {
    n = std::snprintf(
        line, sizeof(line),
        "{\"ts_ms\":%llu,\"stream_id\":\"%s\",\"frame_num\":%llu,\"event\":\"%s\","
        "\"class_id\":%d,\"class\":\"%s\",\"track_id\":%llu,\"confidence\":%.4f,"
        "\"bbox\":[%.2f,%.2f,%.2f,%.2f],\"count\":%s,\"zone\":%s,\"keyframe\":%s}\n",
        static_cast<unsigned long long>(e.ts_ms), stream_id.c_str(),
        static_cast<unsigned long long>(e.frame_num), event_type_str(e.type),
        e.class_id, cls, static_cast<unsigned long long>(e.track_id),
        static_cast<double>(e.confidence), static_cast<double>(e.left),
        static_cast<double>(e.top), static_cast<double>(e.width),
        static_cast<double>(e.height), "null", zone_json.c_str(),
        keyframe_json.c_str());
  } else {
    n = std::snprintf(
        line, sizeof(line),
        "{\"ts_ms\":%llu,\"stream_id\":\"%s\",\"frame_num\":%llu,\"event\":\"%s\","
        "\"class_id\":%d,\"class\":\"%s\",\"track_id\":%llu,\"confidence\":%.4f,"
        "\"bbox\":null,\"count\":%d,\"zone\":%s,\"keyframe\":%s}\n",
        static_cast<unsigned long long>(e.ts_ms), stream_id.c_str(),
        static_cast<unsigned long long>(e.frame_num), event_type_str(e.type),
        e.class_id, cls, static_cast<unsigned long long>(e.track_id),
        static_cast<double>(e.confidence), e.count, zone_json.c_str(),
        keyframe_json.c_str());
  }

  if (n > 0 && n < static_cast<int>(sizeof(line))) {
    if (jsonl_.is_open()) {
      jsonl_.write(line, static_cast<std::streamsize>(n));
    }
    ++written_;
  }
}

uint64_t EventWriter::written() const {
  std::lock_guard<std::mutex> lock(mu_);
  return written_;
}

void EventWriter::flush() {
  std::lock_guard<std::mutex> lock(mu_);
  if (jsonl_.is_open()) {
    jsonl_.flush();
  }
}

}  // namespace events
}  // namespace jetedge
