// MetadataProbe — pad probe that reads NvDsBatchMeta from nvinfer output
// and logs per-frame detection counts (Stage 4).

#pragma once

#include <gst/gst.h>

namespace jetedge {
namespace inference {

// Install a buffer probe on `pad` (expected: nvinfer src pad).
// Returns the probe ID, or 0 on failure.
unsigned long install_metadata_probe(GstPad* pad);

}  // namespace inference
}  // namespace jetedge
