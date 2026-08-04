// Pipeline — multi-stream hardware decode → nvstreammux → nvinfer → [nvtracker]
// → fakesink (Stage 5).
//
// Manages the GStreamer pipeline lifecycle:
//   SourceBin[0..N] → nvstreammux → nvinfer → nvtracker → fakesink (full path)
//   SourceBin[0..N] → nvstreammux → fakesink                         (baseline)

#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <gst/gst.h>

#include "jetedge/control/control_backend.h"
#include "jetedge/control/error_store.h"
#include "jetedge/events/event_engine.h"
#include "jetedge/events/event_writer.h"
#include "jetedge/events/keyframe_writer.h"
#include "jetedge/inference/inference_config.h"
#include "jetedge/llm/llm_config.h"
#include "jetedge/llm/llm_router.h"
#include "jetedge/metrics/metrics_registry.h"
#include "jetedge/pipeline/reconnect_policy.h"
#include "jetedge/pipeline/source_manager.h"
#include "jetedge/pipeline/stream_config.h"
#include "jetedge/scheduler/scheduler_policy.h"
#include "jetedge/scheduler/system_metrics.h"

namespace jetedge {
namespace pipeline {

class Pipeline : public control::ControlBackend {
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
  // events_config: rule events + keyframe extraction (Stage 6).
  // llm_config: async cloud analysis routing (Stage 7).
  // rtsp_config: RTSP source + reconnect settings (Stage 8).
  // scheduler_config: deterministic runtime scheduler (Stage 9).
  bool build(const std::vector<StreamConfig>& stream_configs,
             const MuxConfig& mux_config,
             const inference::InferenceConfig& infer_config = {},
             const TrackerConfig& tracker_config = {},
             const OutputConfig& output_config = {},
             const events::EventsConfig& events_config = {},
             const llm::LlmConfig& llm_config = {},
             const RtspConfig& rtsp_config = {},
             const scheduler::SchedulerConfig& scheduler_config = {});

  // Run the GLib main loop (blocking).  Exits on EOS, error, or signal.
  void run();

  // Request the main loop to quit (thread-safe).
  void quit();

  // Print per-stream frame statistics.
  void print_stats() const;

  // Per-stream metrics summary (Stage 5).
  std::vector<metrics::MetricsRegistry::StreamSummary> metrics_snapshot() const;

  // ---- Stage 11: safe Control API backend -----------------------------------
  // Implemented over the GStreamer/GLib main-loop thread: control threads are
  // never allowed to touch pipeline state directly.  Ops that must read or
  // write main-loop state are marshaled onto the main loop with a bounded
  // wait; ops that fail to marshal return an error instead of hanging.
  std::vector<std::string> stream_ids() const override;
  std::vector<control::StreamStatus> stream_status() const override;
  std::vector<metrics::MetricsRegistry::StreamSummary> metrics_summary() const override;
  // Stage 12: inference-stage latency (metrics registry passthrough).
  metrics::MetricsRegistry::LatencyStats latency_stats(int stream_idx) const override;
  metrics::MetricsRegistry::LatencyStats latency_stats_since(
      int stream_idx, uint64_t since_seq) const override;
  std::vector<uint32_t> latency_samples_since(
      int stream_idx, uint64_t since_seq) const override;
  uint64_t latency_watermark() const override;
  control::SchedulerStatus scheduler_status() const override;
  scheduler::SchedulerConfig scheduler_config() const override;
  std::vector<control::ErrorRecord> recent_errors(size_t limit) const override;
  bool safety_state_allows() const override;
  control::WriteResult set_infer_interval(int stream_idx, int interval) override;
  control::WriteResult set_priority(int stream_idx,
                                    pipeline::StreamPriority priority) override;
  control::WriteResult restart_stream(int stream_idx) override;
  control::ConfigSnapshot current_config_snapshot() const override;
  control::WriteResult apply_snapshot(const control::ConfigSnapshot& snap) override;

  // Record an error into the bounded error store (bus ERROR / RTSP watchdog /
  // control write failures).  Thread-safe; serves GET /errors/recent.
  void record_control_error(const std::string& level, const std::string& stream_id,
                            const std::string& state, const std::string& operation,
                            const std::string& error_code, const std::string& message);

  // ---- Stage 8 RTSP reconnect driver ---------------------------------------
  // Per-stream reconnect bookkeeping, all touched only from the GLib main
  // loop thread (bus watch + watchdog timer).
  struct RtspWatch {
    explicit RtspWatch(const ReconnectPolicy::Params& params) : policy(params) {}
    ReconnectPolicy policy;
    uint64_t connect_ms = 0;          // last mark_connect (monotonic ms)
    uint64_t verify_start_ms = 0;     // FPS verification window open time
    uint64_t verify_start_count = 0;  // frame count at window open
    uint64_t deadline_ms = 0;         // reconnect deadline (kReconnecting)
    uint64_t state_since_ms = 0;      // monotonic ms since current state
    StreamState seen_state = StreamState::kOffline;
    bool verifying = false;           // FPS window currently open
  };

 private:
  static gboolean on_bus_message(GstBus* bus, GstMessage* msg, gpointer user_data);
  static gboolean on_periodic_report(gpointer user_data);
  static gboolean on_llm_metrics_report(gpointer user_data);
  static gboolean on_rtsp_watch(gpointer user_data);
  static gboolean on_scheduler_tick(gpointer user_data);
  static GstPadProbeReturn on_stream_eos(GstPad* pad, GstPadProbeInfo* info,
                                         gpointer user_data);
  void print_metrics_log() const;
  std::string build_metrics_json() const;
  void flush_stream_events(int stream_idx, uint64_t ts_ms);
  void release_resources();

  // Install (or reinstall after a rebuild) per-stream EOS probes → disappearance
  // flush.  Probes are removed from stale pads first; pads that were destroyed
  // by a teardown must already have their probes removed before calling this.
  void install_eos_probes();

  // 1s watchdog: detect stalls, run backoff deadlines, verify post-reconnect
  // input FPS (needs ≥ min_fps over the verify_sec window).
  void tick_rtsp_watch();
  void schedule_reconnect(size_t idx, const char* reason);
  void do_reconnect(size_t idx);
  void log_rtsp_states() const;

  // Stage 9 scheduler driver: sample system metrics → policy update →
  // per-stream inference intervals.  Runs on the GLib main loop at
  // scheduler_config_.sample_interval_sec.
  void tick_scheduler();
  void apply_scheduler_intervals();
  void log_scheduler_report() const;

  // Map a GStreamer object (error source) to a stream index by walking up its
  // parent chain and matching our "src-<id>-*" element names.  -1 = not ours.
  int stream_index_from_object(GstObject* obj) const;

  // ---- Stage 11 control helpers --------------------------------------------
  // Run `fn` on the GLib main loop thread and wait (bounded).  Runs inline
  // when already on the main loop thread or when the loop is not running.
  // Returns false when the wait times out (control op must fail gracefully).
  bool on_main_loop(const std::function<void()>& fn) const;

  control::StreamStatus make_stream_status(size_t idx) const;
  control::SchedulerStatus make_scheduler_status() const;

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
  guint event_probe_id_ = 0;   // on tracker (or nvinfer) src pad (Stage 6)
  guint report_timer_id_ = 0;  // periodic metrics report
  guint llm_metrics_timer_id_ = 0;  // periodic DeepSeek analysis (Stage 7)
  guint rtsp_watch_timer_id_ = 0;  // Stage 8: 1s RTSP stall/reconnect watchdog
  guint scheduler_timer_id_ = 0;   // Stage 9: scheduler driver tick
  GMainLoop* loop_ = nullptr;
  guint bus_watch_id_ = 0;

  // Per-stream EOS probes on each decoder src pad → disappearance flush.
  std::vector<GstPad*> eos_pads_;
  std::vector<guint> eos_probe_ids_;

  std::unique_ptr<SourceManager> source_mgr_;
  std::unique_ptr<metrics::MetricsRegistry> metrics_;
  std::unique_ptr<events::EventEngine> event_engine_;
  std::unique_ptr<events::EventWriter> event_writer_;
  std::unique_ptr<events::KeyframeWriter> keyframe_writer_;
  std::unique_ptr<llm::LlmRouter> llm_router_;  // Stage 7 async cloud analysis
  std::vector<std::string> class_names_;        // for LLM prompt building
  RtspConfig rtsp_config_;                      // Stage 8: RTSP settings
  std::vector<RtspWatch> rtsp_watch_;           // one entry per source

  // Stage 9 scheduler state.  The policy + sampler live here; the per-stream
  // intervals are applied through SourceManager (atomics inside SourceBin,
  // so the streaming thread never blocks).
  scheduler::SchedulerConfig scheduler_config_;
  std::unique_ptr<scheduler::SchedulerPolicy> scheduler_;
  scheduler::SystemSampler sys_sampler_;
  std::vector<StreamConfig> stream_configs_;    // base stream configs
  std::vector<int> scheduler_intervals_;        // last applied per stream
  scheduler::SystemSample scheduler_last_sample_;  // for the periodic report

  // Stage 11 control state.  runtime_priorities_ is the effective priority
  // (control ops may change it; the scheduler tier mapping reads it instead
  // of stream_configs_ priorities).  Touched only on the GLib main loop
  // thread (control ops marshal there).
  std::vector<StreamPriority> runtime_priorities_;
  std::unique_ptr<control::ErrorStore> error_store_;
  GThread* main_loop_thread_ = nullptr;
  std::atomic<bool> loop_running_{false};
};

}  // namespace pipeline
}  // namespace jetedge
