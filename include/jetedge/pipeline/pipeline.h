// Pipeline — multi-stream hardware decode → nvstreammux → nvinfer → [nvtracker]
// → fakesink (Stage 5).
//
// Manages the GStreamer pipeline lifecycle:
//   SourceBin[0..N] → nvstreammux → nvinfer → nvtracker → fakesink (full path)
//   SourceBin[0..N] → nvstreammux → fakesink                         (baseline)

#pragma once

#include <memory>
#include <string>
#include <vector>

#include <gst/gst.h>

#include "jetedge/inference/inference_config.h"
#include "jetedge/metrics/metrics_registry.h"
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
  // infer_config: nvinfer settings (enable=false keeps the baseline path).
  // tracker_config: nvtracker settings (enable=false links nvinfer directly).
  // output_config: JSONL output + labels file settings.
  bool build(const std::vector<StreamConfig>& stream_configs,
             const MuxConfig& mux_config,
             const inference::InferenceConfig& infer_config = {},
             const TrackerConfig& tracker_config = {},
             const OutputConfig& output_config = {});

  // Run the GLib main loop (blocking).  Exits on EOS, error, or signal.
  void run();

  // Request the main loop to quit (thread-safe).
  void quit();

  // Print per-stream frame statistics.
  void print_stats() const;

  // Per-stream metrics summary (Stage 5).
  std::vector<metrics::MetricsRegistry::StreamSummary> metrics_snapshot() const;

 private:
  static gboolean on_bus_message(GstBus* bus, GstMessage* msg, gpointer user_data);
  static gboolean on_periodic_report(gpointer user_data);
  void print_metrics_log() const;
  void release_resources();

  GstElement* pipeline_ = nullptr;
  GstElement* sink_ = nullptr;
  GstElement* nvinfer_ = nullptr;
  GstElement* tracker_ = nullptr;
  GstPad* nvinfer_sink_pad_ = nullptr;
  GstPad* nvinfer_src_pad_ = nullptr;
  GstPad* tracker_src_pad_ = nullptr;
  guint input_probe_id_ = 0;   // on nvinfer sink pad
  guint infer_probe_id_ = 0;   // on nvinfer src pad
  guint output_probe_id_ = 0;  // on tracker (or nvinfer) src pad
  guint report_timer_id_ = 0;  // periodic metrics report
  GMainLoop* loop_ = nullptr;
  guint bus_watch_id_ = 0;

  std::unique_ptr<SourceManager> source_mgr_;
  std::unique_ptr<metrics::MetricsRegistry> metrics_;
};

}  // namespace pipeline
}  // namespace jetedge
