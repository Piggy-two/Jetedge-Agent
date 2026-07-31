// Pipeline — multi-stream hardware decode → nvstreammux → fakesink (Stage 2).
//
// Manages the GStreamer pipeline lifecycle:
//   SourceBin[0..N] → nvstreammux → fakesink

#pragma once

#include <memory>
#include <string>
#include <vector>

#include <gst/gst.h>

#include "jetedge/pipeline/source_manager.h"
#include "jetedge/pipeline/stream_config.h"

namespace jetedge {
namespace pipeline {

class Pipeline {
 public:
  Pipeline() = default;

  // Non-copyable, non-movable.
  Pipeline(const Pipeline&) = delete;
  Pipeline& operator=(const Pipeline&) = delete;
  Pipeline(Pipeline&&) = delete;
  Pipeline& operator=(Pipeline&&) = delete;

  ~Pipeline();

  // Build the pipeline from config.  Returns true on success.
  // stream_configs: per-source configuration (1..N entries).
  // mux_config: nvstreammux settings.
  bool build(const std::vector<StreamConfig>& stream_configs,
             const MuxConfig& mux_config);

  // Run the GLib main loop (blocking).  Exits on EOS, error, or signal.
  void run();

  // Request the main loop to quit (thread-safe).
  void quit();

  // Print per-stream frame statistics.
  void print_stats() const;

 private:
  static gboolean on_bus_message(GstBus* bus, GstMessage* msg, gpointer user_data);
  void release_resources();

  GstElement* pipeline_ = nullptr;
  GstElement* sink_ = nullptr;
  GMainLoop* loop_ = nullptr;
  guint bus_watch_id_ = 0;

  std::unique_ptr<SourceManager> source_mgr_;
};

}  // namespace pipeline
}  // namespace jetedge
