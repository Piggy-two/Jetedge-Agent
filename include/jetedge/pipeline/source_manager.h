// SourceManager — manages multiple SourceBin instances and nvstreammux.
//
// Creates one SourceBin per stream config, requests streammux sink pads,
// links decoder src → streammux sink, and tracks per-stream frame counts.

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
             const MuxConfig& mux_config);

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

  GstElement* streammux() const { return streammux_; }

 private:
  GstElement* streammux_ = nullptr;
  std::vector<std::unique_ptr<SourceBin>> sources_;
};

}  // namespace pipeline
}  // namespace jetedge
