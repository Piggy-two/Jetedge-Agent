// EventProbe — pad probe that feeds the EventEngine (Stage 6).
//
// Installed on the nvtracker (or nvinfer) src pad.  Per frame it:
//   1. adapts NvDsObjectMeta → ObservedObject and calls EventEngine,
//   2. writes the fired EventRecords to the EventWriter (JSONL),
//   3. saves a bounded JPEG keyframe per event via KeyframeWriter.

#pragma once

#include <string>
#include <vector>

#include <gst/gst.h>

#include "jetedge/events/event_engine.h"
#include "jetedge/events/event_writer.h"
#include "jetedge/events/keyframe_writer.h"

namespace jetedge {
namespace llm {
class LlmRouter;  // fwd — enqueues routed events for cloud analysis (Stage 7)
}  // namespace llm

namespace events {

// Install the probe on `pad` (nvtracker or nvinfer src pad).  The probe does
// not take ownership of `pad`; the caller must remove the probe before the
// pad is released.  `writer` may be null (keyframes disabled).  `router`
// may be null (cloud analysis disabled); when non-null, events that match
// the routing table are enqueued for async Qwen / DeepSeek analysis.
// Returns the probe ID or 0 on failure.
unsigned long install_event_probe(GstPad* pad, EventEngine* engine,
                                  KeyframeWriter* writer, EventWriter* event_writer,
                                  llm::LlmRouter* router,
                                  const std::vector<std::string>& stream_ids,
                                  const std::vector<std::string>& class_names);

}  // namespace events
}  // namespace jetedge
