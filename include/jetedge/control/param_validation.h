// ParamValidation — pure validation for Control API write operations (Stage 11).
//
// No I/O, no state: every rule from implementation_plan §59 is a pure
// function so the whole write-op gate is unit-testable without GStreamer.

#pragma once

#include <string>
#include <vector>

#include "jetedge/pipeline/stream_config.h"

namespace jetedge {
namespace control {

// infer_interval must be in [0, max_interval].
bool valid_infer_interval(int interval, int max_interval);

// priority strings: "high" | "normal" | "low" (any other value rejected).
bool valid_priority(const std::string& s);

// First index of `stream_id` in `ids` (mux pad order), or -1 when unknown.
int find_stream_index(const std::vector<std::string>& ids, const std::string& stream_id);

// Priority rank comparison helper: a smaller rank is more important.
// Returns true when `new_rank` is MORE important than `old_rank` (i.e. the
// change increases the stream's load on the system).
bool priority_ranks_up(pipeline::StreamPriority old_prio, pipeline::StreamPriority new_prio);

}  // namespace control
}  // namespace jetedge
