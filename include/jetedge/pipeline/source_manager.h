// SourceManager — manages multiple SourceBin instances and nvstreammux.
//
// Creates one SourceBin per stream config, requests streammux sink pads,
// links decoder src → streammux sink, and tracks per-stream frame counts.
//
// Stage 8: RTSP sources can be rebuilt at runtime (reconnect).  rebuild_source()
// unlinks the decoder pad, releases the streammux request pad, tears the old
// chain down, builds a fresh one and re-requests the same "sink_<idx>" pad so
// stream_id → pad index mapping stays stable.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <gst/gst.h>

#include "jetedge/pipeline/source_bin.h"
#include "jetedge/pipeline/stream_config.h"

namespace jetedge {
namespace pipeline {

class SourceManager {
 public:
  SourceManager() = default;

  // Non-copyable, non-movable.
  SourceManager(const SourceManager&) = delete;
  SourceManager& operator=(const SourceManager&) = delete;
  SourceManager(SourceManager&&) = delete;
  SourceManager& operator=(SourceManager&&) = delete;

  ~SourceManager();

  // Build all source bins + streammux.  Must be called after gst_init.
  // `pipeline` is the parent GstPipeline bin.
  bool build(GstElement* pipeline,
             const std::vector<StreamConfig>& stream_configs,
             const MuxConfig& mux_config,
             const RtspConfig& rtsp_config = {});

  // Rebuild one RTSP source at runtime (Stage 8 reconnect).  Returns true on
  // success; on failure the old source is already torn down and the caller
  // decides when to retry.  Must run in the GLib main loop thread.
  bool rebuild_source(int idx, GstElement* pipeline);

  // Frame counter / last-frame timestamp of source `idx` (stall watchdog).
  uint64_t frame_count(int idx) const;
  uint64_t last_frame_ts_ms(int idx) const;

  // True when source `idx` is an RTSP source (eligible for reconnect).
  bool is_rtsp_source(int idx) const;

  // Number of sources successfully built.
  int source_count() const { return static_cast<int>(sources_.size()); }

  // Get frame counts per source.
  struct SourceStats {
    std::string stream_id;
    uint64_t frame_count = 0;
  };
  std::vector<SourceStats> get_stats() const;

  // Print frame statistics to stdout.
  void print_stats() const;

  // Stream ids in mux pad_index order (index == nvstreammux sink pad index).
  std::vector<std::string> stream_ids() const;

  // Decoder src pad of source `idx` (for per-stream EOS probes), or nullptr.
  GstPad* decoder_src_pad(int idx) const;

  // True when `obj` is one of the current chain elements of source `idx`
  // (rtspsrc/depay/parser/decoder).  Errors from elements that were replaced
  // by a rebuild are stale and must not count as new failures.
  bool is_current_chain_element(int idx, GstObject* obj) const;

  // Stage 9 scheduler: inference interval for source `idx` (see SourceBin).
  // Survives RTSP rebuilds (the SourceBin object is not replaced).
  void set_infer_interval(int idx, int interval);

  // Current inference interval of source `idx` (Stage 11 control API).
  int infer_interval(int idx) const;

  GstElement* streammux() const { return streammux_; }

 private:
  GstElement* streammux_ = nullptr;
  std::vector<std::unique_ptr<SourceBin>> sources_;
  std::vector<StreamConfig> configs_;   // per-source configs (rebuilds)
  RtspConfig rtsp_config_;
};

}  // namespace pipeline
}  // namespace jetedge
