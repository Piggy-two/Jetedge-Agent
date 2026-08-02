// SourceBin — single video source chain.
//
// Internal pipelines:
//   file:  filesrc → [qtdemux] → h264parse → nvv4l2decoder
//   rtsp:  rtspsrc (dynamic pad) → rtph264depay → h264parse → nvv4l2decoder
//          (Stage 8)
//
// The decoder src pad is exposed for external linking (to nvstreammux).
// A pad probe on the decoder src counts emitted frames and records the
// timestamp of the last frame (used by the stall watchdog).
//
// Stage 8: a SourceBin can be torn down and rebuilt while the pipeline keeps
// running (RTSP reconnect).  teardown() must be called from the GLib main
// loop thread; the caller is responsible for unlinking decoder_src_pad()
// from the streammux sink pad first and for removing per-stream probes that
// were installed by other modules (e.g. the EOS probe in Pipeline).

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
  SourceBin(const StreamConfig& config, const RtspConfig& rtsp_config);
  ~SourceBin();

  // Non-copyable, non-movable (owns GStreamer objects).
  SourceBin(const SourceBin&) = delete;
  SourceBin& operator=(const SourceBin&) = delete;
  SourceBin(SourceBin&&) = delete;
  SourceBin& operator=(SourceBin&&) = delete;

  // Build the source chain.  Elements are added to `pipeline` bin.
  // Returns true on success.
  bool build(GstElement* pipeline);

  // Remove all elements from the pipeline bin and reset internal state so
  // build() can be called again (RTSP reconnect).  The caller must unlink
  // decoder_src_pad() from the streammux first.  Idempotent.
  void teardown(GstElement* pipeline);

  // Bring all owned elements to the state of their parent bin.  Only needed
  // after a runtime rebuild (initial build is driven by the pipeline state
  // change); the elements were created at NULL state.
  void sync_state_with_parent();

  bool is_rtsp() const { return config_.type == "rtsp"; }

  // Get the decoder source pad for linking to streammux (nullptr after
  // teardown).
  GstPad* decoder_src_pad() const { return decoder_src_pad_; }

  const std::string& stream_id() const { return config_.id; }
  StreamPriority priority() const { return config_.priority; }

  // Frame counter (incremented by the pad probe).
  uint64_t frame_count() const { return frame_count_.load(); }

  // Monotonic ms of the last decoded frame (0 = none yet).  Stall watchdog
  // uses this to detect a dead RTSP source.
  uint64_t last_frame_ts_ms() const { return last_frame_ts_ms_.load(); }

  // True when `obj` is one of the CURRENT chain elements.  After a rebuild
  // the old elements are destroyed; a bus message whose source is an old
  // element is stale (its error was already handled by the rebuild) and must
  // not count as a new failure.
  bool is_chain_element(GstObject* obj) const {
    return obj == GST_OBJECT(rtsp_src_) || obj == GST_OBJECT(depay_) ||
           obj == GST_OBJECT(filesrc_) || obj == GST_OBJECT(demux_) ||
           obj == GST_OBJECT(parser_) || obj == GST_OBJECT(decoder_);
  }

 private:
  static GstPadProbeReturn on_frame_probe(GstPad* pad, GstPadProbeInfo* info,
                                          gpointer user_data);
  static void on_demux_pad_added(GstElement* src, GstPad* pad, gpointer user_data);
  static void on_rtsp_pad_added(GstElement* src, GstPad* pad, gpointer user_data);

  const StreamConfig config_;
  const RtspConfig rtsp_config_;

  GstElement* rtsp_src_ = nullptr;  // rtspsrc (rtsp type only)
  GstElement* depay_    = nullptr;  // rtph264depay (rtsp type only)
  GstElement* filesrc_  = nullptr;  // filesrc (file type only)
  GstElement* demux_   = nullptr;   // qtdemux (containers only)
  GstElement* parser_  = nullptr;
  GstElement* decoder_ = nullptr;

  GstPad* decoder_src_pad_ = nullptr;  // borrowed reference
  guint frame_probe_id_ = 0;
  std::atomic<uint64_t> frame_count_{0};
  std::atomic<uint64_t> last_frame_ts_ms_{0};
};

}  // namespace pipeline
}  // namespace jetedge
