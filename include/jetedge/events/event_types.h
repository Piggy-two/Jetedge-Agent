// Event types and configuration for the Stage 6 event system.
//
// Pure data types — no GStreamer/DeepStream dependencies, so the event
// engine and its dedup/hysteresis logic are unit-testable directly.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace jetedge {
namespace events {

// Event kinds produced by the rule engine.
enum class EventType {
  kAppearance,    // a new track_id was first observed
  kDisappearance, // a tracked object absent for disappear_grace_frames
  kCountHigh,     // active objects of a class reached count_threshold
  kCountExit,     // active objects dropped below threshold - hysteresis
  kZoneEntry,     // an object's bbox center entered a configured zone
};

inline const char* event_type_str(EventType t) {
  switch (t) {
    case EventType::kAppearance:    return "appearance";
    case EventType::kDisappearance: return "disappearance";
    case EventType::kCountHigh:     return "count_high";
    case EventType::kCountExit:     return "count_exit";
    case EventType::kZoneEntry:     return "zone_entry";
  }
  return "unknown";
}

// One observed object in one frame (adapted from NvDsObjectMeta by the probe).
// Coordinates are in nvstreammux output space.
struct ObservedObject {
  uint64_t track_id = 0;
  int class_id = -1;
  float confidence = 0.0f;
  float left = 0.0f;
  float top = 0.0f;
  float width = 0.0f;
  float height = 0.0f;
};

// A rule zone: an axis-aligned rectangle in mux output space.
struct ZoneRule {
  std::string name;
  std::string stream_id;  // "" = applies to every stream
  float left = 0.0f;
  float top = 0.0f;
  float width = 0.0f;
  float height = 0.0f;
};

// Event engine configuration (parsed from the YAML "events:" section).
struct EventsConfig {
  bool enable = false;
  std::string jsonl_path;               // event JSONL output file
  std::string keyframe_dir;             // keyframe cache directory
  int max_keyframes = 100;              // global cap (0 = unlimited)
  int jpeg_quality = 85;                // 1..100
  uint64_t disappear_grace_frames = 15; // unseen frames before disappearance
  int count_threshold = 3;              // count_high when active count >= this
  int count_hysteresis = 1;             // re-arm when count <= threshold - this
  std::vector<int> classes;             // watched class ids (empty = all)
  std::vector<ZoneRule> zones;          // zone rules
};

// One emitted event.
struct EventRecord {
  EventType type = EventType::kAppearance;
  int stream_idx = -1;
  uint64_t frame_num = 0;
  uint64_t ts_ms = 0;
  uint64_t track_id = 0;
  int class_id = -1;
  float confidence = 0.0f;
  float left = 0.0f;
  float top = 0.0f;
  float width = 0.0f;
  float height = 0.0f;  // appearance / disappearance / zone events
  int count = 0;        // count events
  std::string zone;     // zone name for zone_entry
};

}  // namespace events
}  // namespace jetedge
