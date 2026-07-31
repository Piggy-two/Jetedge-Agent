// SourceBin — single video source chain.
//
// Internal pipeline:
//   filesrc → [qtdemux] → h264parse → nvv4l2decoder
//
// The decoder src pad is exposed for external linking (to nvstreammux).
// A pad probe on the decoder src counts emitted frames.

#pragma once

#include <atomic>
#include <cstdint>
#include <string>

#include <gst/gst.h>

#include "jetedge/pipeline/stream_config.h"

namespace jetedge {
namespace pipeline {

class SourceBin {
 public:
  explicit SourceBin(const StreamConfig& config);
  ~SourceBin();

  // Non-copyable, non-movable (owns GStreamer objects).
  SourceBin(const SourceBin&) = delete;
  SourceBin& operator=(const SourceBin&) = delete;
  SourceBin(SourceBin&&) = delete;
  SourceBin& operator=(SourceBin&&) = delete;

  // Build the source chain.  Elements are added to `pipeline` bin.
  // Returns true on success.
  bool build(GstElement* pipeline);

  // Get the decoder source pad for linking to streammux.
  GstPad* decoder_src_pad() const { return decoder_src_pad_; }

  const std::string& stream_id() const { return config_.id; }
  StreamPriority priority() const { return config_.priority; }

  // Frame counter (incremented by the pad probe).
  uint64_t frame_count() const { return frame_count_.load(); }

 private:
  static GstPadProbeReturn on_frame_probe(GstPad* pad, GstPadProbeInfo* info,
                                          gpointer user_data);
  static void on_demux_pad_added(GstElement* src, GstPad* pad, gpointer user_data);

  const StreamConfig config_;

  GstElement* filesrc_ = nullptr;
  GstElement* demux_   = nullptr;
  GstElement* parser_  = nullptr;
  GstElement* decoder_ = nullptr;

  GstPad* decoder_src_pad_ = nullptr;  // borrowed reference
  std::atomic<uint64_t> frame_count_{0};
};

}  // namespace pipeline
}  // namespace jetedge
