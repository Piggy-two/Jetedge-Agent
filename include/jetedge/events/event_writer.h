// EventWriter — appends one JSONL line per EventRecord (Stage 6).
//
// Line shape (keys stable):
//   {"ts_ms","stream_id","frame_num","event","class_id","class","track_id",
//    "confidence","bbox":[l,t,w,h],"count","zone","keyframe"}
// count/zone/keyframe are null unless the event type carries them.

#pragma once

#include <cstdint>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#include "jetedge/events/event_types.h"

namespace jetedge {
namespace events {

class EventWriter {
 public:
  // stream_ids maps stream index → id; class_names maps class_id → name.
  // Empty path disables file output (events still counted in memory).
  bool open(const std::string& path, const std::vector<std::string>& stream_ids,
            const std::vector<std::string>& class_names);

  // Write one event line.  keyframe_path is the saved keyframe file name
  // (relative to the keyframe dir) or empty.
  void write(const EventRecord& e, const std::string& keyframe_path);

  uint64_t written() const;
  void flush();

 private:
  std::ofstream jsonl_;
  std::vector<std::string> stream_ids_;
  std::vector<std::string> class_names_;
  uint64_t written_ = 0;
  mutable std::mutex mu_;
};

}  // namespace events
}  // namespace jetedge
