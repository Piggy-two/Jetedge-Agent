// Unit tests for the Stage 6 event engine (pure logic, no GStreamer).
//
// Build/run:
//   cmake --build build -j2 && ./build/test_event_engine
//   (also registered with CTest: ctest --test-dir build)

#include <cstdio>
#include <cstdlib>
#include <vector>

#include "jetedge/events/event_engine.h"

using jetedge::events::EventEngine;
using jetedge::events::EventRecord;
using jetedge::events::EventsConfig;
using jetedge::events::EventType;
using jetedge::events::ObservedObject;
using jetedge::events::ZoneRule;

namespace {

int g_failures = 0;

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
      ++g_failures;                                                          \
    }                                                                        \
  } while (0)

EventsConfig base_config() {
  EventsConfig cfg;
  cfg.disappear_grace_frames = 10;
  cfg.count_threshold = 3;
  cfg.count_hysteresis = 1;
  cfg.classes = {0, 2};  // person, car
  return cfg;
}

ObservedObject obj(uint64_t track_id, int class_id, float left, float top,
                   float width = 50.0f, float height = 80.0f) {
  ObservedObject o;
  o.track_id = track_id;
  o.class_id = class_id;
  o.left = left;
  o.top = top;
  o.width = width;
  o.height = height;
  o.confidence = 0.9f;
  return o;
}

bool has_event(const std::vector<EventRecord>& events, EventType type) {
  for (const auto& e : events) {
    if (e.type == type) return true;
  }
  return false;
}

const EventRecord* find_event(const std::vector<EventRecord>& events, EventType type) {
  for (const auto& e : events) {
    if (e.type == type) return &e;
  }
  return nullptr;
}

// 1. Appearance fires exactly once per new track.
void test_appearance_once() {
  EventsConfig cfg = base_config();
  EventEngine eng(cfg, {"cam1"});
  auto e1 = eng.process_frame(0, 1, 1000, {obj(7, 2, 10, 10)});
  CHECK(e1.size() == 1 && e1[0].type == EventType::kAppearance);
  CHECK(e1[0].track_id == 7 && e1[0].class_id == 2);
  // Same track in later frames: no duplicate appearance.
  auto e2 = eng.process_frame(0, 2, 1033, {obj(7, 2, 12, 12)});
  CHECK(e2.empty());
  // A second new track fires again.
  auto e3 = eng.process_frame(0, 3, 1066, {obj(8, 2, 20, 20)});
  CHECK(e3.size() == 1 && e3[0].type == EventType::kAppearance && e3[0].track_id == 8);
}

// 2. Unwatched classes produce no events but do not disturb state.
void test_class_filter() {
  EventsConfig cfg = base_config();  // watches 0 (person) and 2 (car)
  EventEngine eng(cfg, {"cam1"});
  // bicycle (class 1) is not watched.
  auto e1 = eng.process_frame(0, 1, 1000, {obj(9, 1, 10, 10)});
  CHECK(e1.empty());
  auto e2 = eng.process_frame(0, 2, 1033, {});
  CHECK(e2.empty());  // still no events
}

// 3. Disappearance after the grace period, exactly once, with the last
//    known bbox and last-seen frame.
void test_disappearance_grace() {
  EventsConfig cfg = base_config();
  EventEngine eng(cfg, {"cam1"});
  eng.process_frame(0, 1, 1000, {obj(7, 2, 10, 10, 50, 80)});
  // Absent for 9 frames (< grace 10): no event.
  auto e2 = eng.process_frame(0, 10, 4000, {});
  CHECK(!has_event(e2, EventType::kDisappearance));
  // Absent for 10 frames (frame 11 - 1 = 10): disappearance.
  auto e3 = eng.process_frame(0, 11, 4033, {});
  const EventRecord* d = find_event(e3, EventType::kDisappearance);
  CHECK(d != nullptr);
  if (d) {
    CHECK(d->track_id == 7 && d->class_id == 2);
    CHECK(d->frame_num == 1);  // last seen frame
    CHECK(d->left == 10.0f && d->top == 10.0f && d->width == 50.0f);
  }
  // No second disappearance.
  auto e4 = eng.process_frame(0, 12, 4066, {});
  CHECK(!has_event(e4, EventType::kDisappearance));
}

// 4. Re-appearance of a vanished track id fires appearance again.
void test_track_reappearance() {
  EventsConfig cfg = base_config();
  EventEngine eng(cfg, {"cam1"});
  eng.process_frame(0, 1, 1000, {obj(7, 2, 10, 10)});
  eng.process_frame(0, 11, 4033, {});  // disappearance
  auto e = eng.process_frame(0, 12, 4066, {obj(7, 2, 10, 10)});
  const EventRecord* a = find_event(e, EventType::kAppearance);
  CHECK(a != nullptr && a->track_id == 7);
}

// 5. Count hysteresis: high at threshold, no re-fire while high,
//    exit after drop, re-fire after re-arm.
void test_count_hysteresis() {
  EventsConfig cfg = base_config();  // threshold 3, hysteresis 1
  EventEngine eng(cfg, {"cam1"});
  auto e1 = eng.process_frame(0, 1, 1000,
                              {obj(1, 2, 10, 10), obj(2, 2, 20, 20), obj(3, 2, 30, 30)});
  const EventRecord* h = find_event(e1, EventType::kCountHigh);
  CHECK(h != nullptr && h->count == 3 && h->class_id == 2);
  // Still 3 (or 4) objects: no second count_high.
  auto e2 = eng.process_frame(0, 2, 1033,
                              {obj(1, 2, 10, 10), obj(2, 2, 20, 20), obj(3, 2, 30, 30),
                               obj(4, 2, 40, 40)});
  CHECK(!has_event(e2, EventType::kCountHigh));
  // Drop to 2 (<= threshold - hysteresis = 2): count_exit.
  auto e3 = eng.process_frame(0, 3, 1066,
                              {obj(1, 2, 10, 10), obj(2, 2, 20, 20)});
  const EventRecord* x = find_event(e3, EventType::kCountExit);
  CHECK(x != nullptr && x->count == 2);
  // Count stays 2: no repeat exit.
  auto e4 = eng.process_frame(0, 4, 1100,
                              {obj(1, 2, 10, 10), obj(2, 2, 20, 20)});
  CHECK(!has_event(e4, EventType::kCountExit));
  // Back to 3: re-armed, fires again.
  auto e5 = eng.process_frame(0, 5, 1133,
                              {obj(1, 2, 10, 10), obj(2, 2, 20, 20), obj(3, 2, 30, 30)});
  CHECK(has_event(e5, EventType::kCountHigh));
  // Counts are per class.
  auto e6 = eng.process_frame(0, 6, 1166, {obj(5, 0, 10, 10)});  // person
  CHECK(!has_event(e6, EventType::kCountHigh));
}

// 6. Zone entry: once per track per zone, bbox-center based, stream filter.
void test_zone_entry() {
  EventsConfig cfg = base_config();
  ZoneRule road;
  road.name = "road";
  road.stream_id = "cam1";  // only applies to cam1
  road.left = 0;
  road.top = 400;
  road.width = 1280;
  road.height = 300;
  cfg.zones.push_back(road);
  EventEngine eng(cfg, {"cam1", "cam2"});

  // cam1, bbox center (25+50/2=50, 400+80/2=440) → inside road.
  auto e1 = eng.process_frame(0, 1, 1000, {obj(7, 2, 0, 400, 50, 80)});
  const EventRecord* z = find_event(e1, EventType::kZoneEntry);
  CHECK(z != nullptr && z->zone == "road");
  // Same track still in zone: no repeat.
  auto e2 = eng.process_frame(0, 2, 1033, {obj(7, 2, 10, 420, 50, 80)});
  CHECK(!has_event(e2, EventType::kZoneEntry));
  // Different track in zone: fires.
  auto e3 = eng.process_frame(0, 3, 1066, {obj(8, 2, 100, 430, 50, 80)});
  CHECK(has_event(e3, EventType::kZoneEntry));
  // cam2 (stream_idx 1): zone is cam1-only, no event even with center in
  // the same rectangle.
  auto e4 = eng.process_frame(1, 1, 2000, {obj(9, 2, 100, 430, 50, 80)});
  CHECK(!has_event(e4, EventType::kZoneEntry));

  // Boundary: center at left edge is inside (>= left), at left+width is out.
  EventsConfig cfg2 = base_config();
  cfg2.zones.push_back(road);
  EventEngine eng2(cfg2, {"cam1"});
  auto e5 = eng2.process_frame(0, 1, 1000, {obj(1, 2, 0, 400, 0, 0)});   // center (0,400): inside
  CHECK(has_event(e5, EventType::kZoneEntry));
  auto e6 = eng2.process_frame(0, 2, 1033, {obj(2, 2, 1280, 400, 0, 0)}); // center (1280,400): out
  CHECK(!has_event(e6, EventType::kZoneEntry));

  // A zone without stream_id applies to every stream.
  EventsConfig cfg3 = base_config();
  ZoneRule all;
  all.name = "any";
  all.left = 0;
  all.top = 0;
  all.width = 100;
  all.height = 100;
  cfg3.zones.push_back(all);
  EventEngine eng3(cfg3, {"cam1", "cam2"});
  auto e7 = eng3.process_frame(1, 1, 2000, {obj(9, 2, 10, 10, 20, 20)});
  const EventRecord* z2 = find_event(e7, EventType::kZoneEntry);
  CHECK(z2 != nullptr && z2->zone == "any");
}

// 7. flush_stream emits disappearance for all remaining tracks, once.
void test_flush_stream() {
  EventsConfig cfg = base_config();
  EventEngine eng(cfg, {"cam1"});
  eng.process_frame(0, 1, 1000, {obj(7, 2, 10, 10), obj(8, 2, 20, 20)});
  auto fl = eng.flush_stream(0, 5000);
  CHECK(fl.size() == 2);
  for (const auto& e : fl) {
    CHECK(e.type == EventType::kDisappearance);
    CHECK(e.ts_ms == 5000);
  }
  // Second flush: nothing new.
  auto fl2 = eng.flush_stream(0, 6000);
  CHECK(fl2.empty());
}

// 8. Per-stream counters accumulate.
void test_counters() {
  EventsConfig cfg = base_config();
  EventEngine eng(cfg, {"cam1", "cam2"});
  eng.process_frame(0, 1, 1000, {obj(7, 2, 10, 10)});
  eng.process_frame(0, 11, 4033, {});
  eng.flush_stream(1, 5000);
  const auto counts = eng.stream_counters();
  CHECK(counts.size() == 2);
  CHECK(counts[0].appearance == 1 && counts[0].disappearance == 1);
  CHECK(counts[1].disappearance == 0);  // no tracks on cam2
}

}  // namespace

int main() {
  test_appearance_once();
  test_class_filter();
  test_disappearance_grace();
  test_track_reappearance();
  test_count_hysteresis();
  test_zone_entry();
  test_flush_stream();
  test_counters();

  if (g_failures == 0) {
    std::printf("ALL PASS\n");
    return EXIT_SUCCESS;
  }
  std::printf("%d FAILURE(S)\n", g_failures);
  return EXIT_FAILURE;
}
