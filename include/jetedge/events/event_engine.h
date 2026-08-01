// EventEngine — deterministic rule engine over per-stream track state
// (Stage 6).
//
// Pure C++ logic with no GStreamer/DeepStream dependencies so the dedup and
// hysteresis state machines can be unit-tested directly.
//
// Rules (all per-stream, restricted to the configured watched classes):
//   appearance     — first time a track_id is observed
//   disappearance  — a known track_id absent for disappear_grace_frames
//   count_high     — active objects of a class >= count_threshold; latched
//                    until the count falls below threshold - hysteresis
//   count_exit     — the re-arm transition of count_high
//   zone_entry     — bbox center enters a configured zone (once per track/zone)
//
// Dedup is state-based (per-track / per-zone / hysteresis latch), not
// timer-based, so behavior is deterministic and testable.

#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "jetedge/events/event_types.h"

namespace jetedge {
namespace events {

class EventEngine {
 public:
  // `stream_ids` maps stream index → stream id (mux pad_index order); used
  // to apply per-stream zone rules.
  EventEngine(EventsConfig config, const std::vector<std::string>& stream_ids);

  // Process one frame of one stream.  Returns newly fired events in a
  // deterministic order: disappearance, then per-object (appearance, zone),
  // then count transitions.
  std::vector<EventRecord> process_frame(int stream_idx, uint64_t frame_num,
                                         uint64_t ts_ms,
                                         const std::vector<ObservedObject>& objects);

  // Fire disappearance for every still-tracked object of a stream
  // (per-stream EOS or shutdown).  Returns the events.
  std::vector<EventRecord> flush_stream(int stream_idx, uint64_t ts_ms);

  struct EventCounters {
    uint64_t appearance = 0;
    uint64_t disappearance = 0;
    uint64_t count_high = 0;
    uint64_t count_exit = 0;
    uint64_t zone_entry = 0;
    uint64_t total() const {
      return appearance + disappearance + count_high + count_exit + zone_entry;
    }
  };

  // Per-stream counters, indexed by stream index.  Thread-safe.
  std::vector<EventCounters> stream_counters() const;

 private:
  struct TrackState {
    int class_id = -1;
    uint64_t last_seen_frame = 0;
    uint64_t last_seen_ns = 0;
    float left = 0.0f;
    float top = 0.0f;
    float width = 0.0f;
    float height = 0.0f;
    float confidence = 0.0f;
    bool disappearance_fired = false;
    std::vector<std::string> zones_fired;
  };

  struct StreamState {
    std::unordered_map<uint64_t, TrackState> tracks;
    std::unordered_map<int, bool> count_high_fired;  // class → latch
  };

  StreamState& stream_state(int stream_idx);
  bool watches_class(int class_id) const;
  bool zone_applies(const ZoneRule& zone, int stream_idx) const;

  EventsConfig config_;
  std::vector<std::string> stream_ids_;
  std::vector<StreamState> streams_;
  std::vector<EventCounters> counters_;
  mutable std::mutex mu_;  // streaming thread + EOS probes + report timer
};

}  // namespace events
}  // namespace jetedge
