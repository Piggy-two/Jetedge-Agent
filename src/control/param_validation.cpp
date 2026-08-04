#include "jetedge/control/param_validation.h"

namespace jetedge {
namespace control {

bool valid_infer_interval(int interval, int max_interval) {
  return interval >= 0 && interval <= max_interval;
}

bool valid_priority(const std::string& s) {
  return s == "high" || s == "normal" || s == "low";
}

int find_stream_index(const std::vector<std::string>& ids, const std::string& stream_id) {
  for (size_t i = 0; i < ids.size(); ++i) {
    if (ids[i] == stream_id) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

bool priority_ranks_up(pipeline::StreamPriority old_prio, pipeline::StreamPriority new_prio) {
  // kHigh=0, kNormal=1, kLow=2 — a smaller value is a higher rank.
  return static_cast<int>(new_prio) < static_cast<int>(old_prio);
}

}  // namespace control
}  // namespace jetedge
