// EventEngine implementation — see event_engine.h for the rule contract.

#include "jetedge/events/event_engine.h"

#include <algorithm>
#include <unordered_set>

#include "jetedge/common/logging.h"

namespace jetedge {
namespace events {

EventEngine::EventEngine(EventsConfig config, const std::vector<std::string>& stream_ids)
    : config_(std::move(config)), stream_ids_(stream_ids) {
  // Pre-size per-stream state so counters are complete for every configured
  // stream even before its first frame.
  streams_.resize(stream_ids_.size());
  counters_.resize(stream_ids_.size());
}

EventEngine::StreamState& EventEngine::stream_state(int stream_idx) {
  if (stream_idx < 0) {
    stream_idx = 0;
  }
  while (static_cast<int>(streams_.size()) <= stream_idx) {
    streams_.emplace_back();
    counters_.emplace_back();
  }
  return streams_[static_cast<size_t>(stream_idx)];
}

bool EventEngine::watches_class(int class_id) const {
  if (config_.classes.empty()) {
    return true;  // watch all classes
  }
  return std::find(config_.classes.begin(), config_.classes.end(), class_id) !=
         config_.classes.end();
}

bool EventEngine::zone_applies(const ZoneRule& zone, int stream_idx) const {
  if (zone.stream_id.empty()) {
    return true;  // applies to every stream
  }
  if (stream_idx < 0 || stream_idx >= static_cast<int>(stream_ids_.size())) {
    return false;
  }
  return zone.stream_id == stream_ids_[static_cast<size_t>(stream_idx)];
}

std::vector<EventRecord> EventEngine::process_frame(
    int stream_idx, uint64_t frame_num, uint64_t ts_ms,
    const std::vector<ObservedObject>& objects) {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<EventRecord> events;
  StreamState& ss = stream_state(stream_idx);
  EventCounters& cnt = counters_[static_cast<size_t>(stream_idx)];

  // Track ids seen in this frame.
  std::unordered_set<uint64_t> seen;
  seen.reserve(objects.size());
  for (const auto& o : objects) {
    seen.insert(o.track_id);
  }

  // 1. Disappearances: known tracks not seen this frame for the grace period.
  for (auto& entry : ss.tracks) {
    TrackState& ts = entry.second;
    if (ts.disappearance_fired || seen.count(entry.first) != 0) {
      continue;
    }
    if (frame_num - ts.last_seen_frame < config_.disappear_grace_frames) {
      continue;
    }
    ts.disappearance_fired = true;
    EventRecord e;
    e.type = EventType::kDisappearance;
    e.stream_idx = stream_idx;
    e.frame_num = ts.last_seen_frame;  // the last frame the object was seen
    e.ts_ms = ts.last_seen_ns;
    e.track_id = entry.first;
    e.class_id = ts.class_id;
    e.confidence = ts.confidence;
    e.left = ts.left;
    e.top = ts.top;
    e.width = ts.width;
    e.height = ts.height;
    events.push_back(std::move(e));
    ++cnt.disappearance;
  }

  // 2. Per-object state update, appearance, and zone entry.
  for (const auto& o : objects) {
    if (!watches_class(o.class_id)) {
      continue;
    }
    auto it = ss.tracks.find(o.track_id);
    if (it != ss.tracks.end() && it->second.disappearance_fired) {
      // A track that already disappeared is a fresh appearance if its id
      // comes back (tracker id reuse).
      ss.tracks.erase(it);
      it = ss.tracks.end();
    }
    if (it == ss.tracks.end()) {
      // New track → appearance.
      TrackState ts;
      ts.class_id = o.class_id;
      ts.last_seen_frame = frame_num;
      ts.last_seen_ns = ts_ms;
      ts.left = o.left;
      ts.top = o.top;
      ts.width = o.width;
      ts.height = o.height;
      ts.confidence = o.confidence;
      it = ss.tracks.emplace(o.track_id, std::move(ts)).first;

      EventRecord e;
      e.type = EventType::kAppearance;
      e.stream_idx = stream_idx;
      e.frame_num = frame_num;
      e.ts_ms = ts_ms;
      e.track_id = o.track_id;
      e.class_id = o.class_id;
      e.confidence = o.confidence;
      e.left = o.left;
      e.top = o.top;
      e.width = o.width;
      e.height = o.height;
      events.push_back(std::move(e));
      ++cnt.appearance;
    } else {
      TrackState& ts = it->second;
      ts.class_id = o.class_id;
      ts.last_seen_frame = frame_num;
      ts.last_seen_ns = ts_ms;
      ts.left = o.left;
      ts.top = o.top;
      ts.width = o.width;
      ts.height = o.height;
      ts.confidence = o.confidence;
    }

    // Zone entry: bbox center in a configured zone, once per track per zone.
    const float cx = o.left + o.width / 2.0f;
    const float cy = o.top + o.height / 2.0f;
    for (const auto& z : config_.zones) {
      if (!zone_applies(z, stream_idx)) {
        continue;
      }
      if (cx < z.left || cx >= z.left + z.width || cy < z.top ||
          cy >= z.top + z.height) {
        continue;
      }
      TrackState& ts = it->second;
      if (std::find(ts.zones_fired.begin(), ts.zones_fired.end(), z.name) !=
          ts.zones_fired.end()) {
        continue;  // already fired for this track/zone
      }
      ts.zones_fired.push_back(z.name);
      EventRecord e;
      e.type = EventType::kZoneEntry;
      e.stream_idx = stream_idx;
      e.frame_num = frame_num;
      e.ts_ms = ts_ms;
      e.track_id = o.track_id;
      e.class_id = o.class_id;
      e.confidence = o.confidence;
      e.left = o.left;
      e.top = o.top;
      e.width = o.width;
      e.height = o.height;
      e.zone = z.name;
      events.push_back(std::move(e));
      ++cnt.zone_entry;
    }
  }

  // 3. Count transitions per watched class (hysteresis latch).
  std::unordered_map<int, int> counts;
  for (const auto& o : objects) {
    if (watches_class(o.class_id)) {
      ++counts[o.class_id];
    }
  }
  for (const auto& entry : counts) {
    const int class_id = entry.first;
    const int n = entry.second;
    const bool fired = ss.count_high_fired[class_id];
    if (!fired && n >= config_.count_threshold) {
      ss.count_high_fired[class_id] = true;
      EventRecord e;
      e.type = EventType::kCountHigh;
      e.stream_idx = stream_idx;
      e.frame_num = frame_num;
      e.ts_ms = ts_ms;
      e.class_id = class_id;
      e.count = n;
      events.push_back(std::move(e));
      ++cnt.count_high;
    } else if (fired && n <= config_.count_threshold - config_.count_hysteresis) {
      ss.count_high_fired[class_id] = false;
      EventRecord e;
      e.type = EventType::kCountExit;
      e.stream_idx = stream_idx;
      e.frame_num = frame_num;
      e.ts_ms = ts_ms;
      e.class_id = class_id;
      e.count = n;
      events.push_back(std::move(e));
      ++cnt.count_exit;
    }
  }

  return events;
}

std::vector<EventRecord> EventEngine::flush_stream(int stream_idx, uint64_t ts_ms) {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<EventRecord> events;
  if (stream_idx < 0 || stream_idx >= static_cast<int>(streams_.size())) {
    return events;
  }
  StreamState& ss = streams_[static_cast<size_t>(stream_idx)];
  EventCounters& cnt = counters_[static_cast<size_t>(stream_idx)];
  for (auto& entry : ss.tracks) {
    TrackState& ts = entry.second;
    if (ts.disappearance_fired) {
      continue;
    }
    ts.disappearance_fired = true;
    EventRecord e;
    e.type = EventType::kDisappearance;
    e.stream_idx = stream_idx;
    e.frame_num = ts.last_seen_frame;
    e.ts_ms = ts_ms;  // EOS/shutdown time (no frame clock available)
    e.track_id = entry.first;
    e.class_id = ts.class_id;
    e.confidence = ts.confidence;
    e.left = ts.left;
    e.top = ts.top;
    e.width = ts.width;
    e.height = ts.height;
    events.push_back(std::move(e));
    ++cnt.disappearance;
  }
  return events;
}

std::vector<EventEngine::EventCounters> EventEngine::stream_counters() const {
  std::lock_guard<std::mutex> lock(mu_);
  return counters_;
}

}  // namespace events
}  // namespace jetedge
