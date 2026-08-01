// MetadataProbe — pad probes for structured output and per-stream metrics
// (Stage 5).
//
//   input probe   (nvinfer sink pad): counts frames entering inference.
//   infer probe   (nvinfer src pad):  counts frames leaving inference.
//   output probe  (nvtracker src pad): emits one JSONL line per detection with
//                                      stream_id / track_id / class /
//                                      confidence / bbox, and updates metrics.

#pragma once

#include <string>
#include <vector>

#include <gst/gst.h>

#include "jetedge/metrics/metrics_registry.h"

namespace jetedge {
namespace inference {

// Load a labelfile into an id→name map.  Supports both DeepStream
// "<id> <name>" lines (ids may be sparse) and plain name lists where the
// 0-based line index is the id.  Returns false on I/O error.
bool load_label_file(const std::string& path, std::vector<std::string>& names_out);

// Install the input probe on the nvinfer sink pad.  Returns probe ID or 0.
unsigned long install_input_probe(GstPad* pad, metrics::MetricsRegistry* metrics);

// Install the inference probe on the nvinfer src pad.  Returns probe ID or 0.
unsigned long install_infer_probe(GstPad* pad, metrics::MetricsRegistry* metrics);

// Install the output probe on the nvtracker src pad.
// stream_ids maps pad_index → stream_id; class_names maps class_id → name.
// jsonl_path empty disables file output.
unsigned long install_output_probe(GstPad* pad,
                                   metrics::MetricsRegistry* metrics,
                                   const std::vector<std::string>& stream_ids,
                                   const std::vector<std::string>& class_names,
                                   const std::string& jsonl_path);

}  // namespace inference
}  // namespace jetedge
