// Pipeline implementation — decode → nvstreammux → nvinfer → nvtracker →
// fakesink (Stage 5) with rule events + keyframe extraction (Stage 6).

#include "jetedge/pipeline/pipeline.h"

#include <algorithm>
#include <cstring>
#include <ctime>

#include <json/json.h>

#include "jetedge/common/logging.h"
#include "jetedge/events/event_probe.h"
#include "jetedge/inference/metadata_probe.h"

namespace jetedge {
namespace pipeline {

namespace {

uint64_t now_ms() {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000ULL +
         static_cast<uint64_t>(ts.tv_nsec / 1000000);
}

uint64_t mono_ms() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000ULL +
         static_cast<uint64_t>(ts.tv_nsec / 1000000);
}

}  // namespace

Pipeline::~Pipeline() {
  release_resources();
}

bool Pipeline::build(const std::vector<StreamConfig>& stream_configs,
                     const MuxConfig& mux_config,
                     const inference::InferenceConfig& infer_config,
                     const TrackerConfig& tracker_config,
                     const OutputConfig& output_config,
                     const events::EventsConfig& events_config,
                     const llm::LlmConfig& llm_config,
                     const RtspConfig& rtsp_config,
                     const scheduler::SchedulerConfig& scheduler_config) {
  if (stream_configs.empty()) {
    LOG_ERROR("pipeline", "", "build", "PIPE001", "%s", "no streams configured");
    return false;
  }
  rtsp_config_ = rtsp_config;
  stream_configs_ = stream_configs;  // Stage 9: priorities for tier mapping
  scheduler_config_ = scheduler_config;

  // 1. Create pipeline bin.
  pipeline_ = gst_pipeline_new("jetedge-pipeline");
  if (!pipeline_) {
    LOG_ERROR("pipeline", "", "build", "PIPE002", "%s", "failed to create pipeline");
    return false;
  }

  // 2. Create fakesink.
  sink_ = gst_element_factory_make("fakesink", "fake-sink");
  if (!sink_) {
    LOG_ERROR("pipeline", "", "build", "PIPE003", "%s", "failed to create fakesink");
    return false;
  }
  g_object_set(G_OBJECT(sink_), "sync", FALSE, nullptr);
  gst_bin_add(GST_BIN(pipeline_), sink_);

  // 3. Build sources + streammux.
  source_mgr_ = std::make_unique<SourceManager>();
  if (!source_mgr_->build(pipeline_, stream_configs, mux_config, rtsp_config_)) {
    LOG_ERROR("pipeline", "", "build", "PIPE004", "%s", "source manager build failed");
    return false;
  }

  // 3b. Stage 8: one reconnect policy per source (used only for RTSP types).
  // Kept in mux pad order == stream_ids() order.
  {
    ReconnectPolicy::Params pp;
    pp.backoff_base_ms = rtsp_config_.backoff_base_ms;
    pp.backoff_max_ms = rtsp_config_.backoff_max_ms;
    pp.max_consecutive_failures = rtsp_config_.max_retries;
    rtsp_watch_.clear();
    for (size_t i = 0; i < stream_configs.size(); ++i) {
      rtsp_watch_.emplace_back(pp);
    }
    const bool has_rtsp = std::any_of(stream_configs.begin(), stream_configs.end(),
                                      [](const StreamConfig& sc) { return sc.type == "rtsp"; });
    if (rtsp_config_.enable && has_rtsp) {
      LOG_INFO("pipeline", "RTSP reconnect watchdog enabled (watch=%ds backoff=%d..%dms "
               "retries=%d verify=%ds min_fps=%.1f)",
               rtsp_config_.watch_timeout_sec, rtsp_config_.backoff_base_ms,
               rtsp_config_.backoff_max_ms, rtsp_config_.max_retries,
               rtsp_config_.verify_sec, rtsp_config_.min_fps);
    }
  }

  // 4. Per-stream metrics registry (pad_index order == mux sink order).
  metrics_ = std::make_unique<metrics::MetricsRegistry>();
  for (const auto& id : source_mgr_->stream_ids()) {
    metrics_->register_stream(id);
  }

  // 5. Link streammux → [nvinfer] → [nvtracker] → fakesink.
  GstElement* upstream = source_mgr_->streammux();
  if (infer_config.enable) {
    if (infer_config.nvinfer_config_path.empty()) {
      LOG_ERROR("pipeline", "", "build", "INFER001", "%s",
                "inference enabled but config_file_path is empty");
      return false;
    }

    nvinfer_ = gst_element_factory_make("nvinfer", "primary-infer");
    if (!nvinfer_) {
      LOG_ERROR("pipeline", "", "build", "INFER002", "%s",
                "failed to create nvinfer element");
      return false;
    }
    g_object_set(G_OBJECT(nvinfer_),
                 "config-file-path", infer_config.nvinfer_config_path.c_str(),
                 "unique-id", infer_config.gie_unique_id,
                 nullptr);
    gst_bin_add(GST_BIN(pipeline_), nvinfer_);

    if (!gst_element_link(upstream, nvinfer_)) {
      LOG_ERROR("pipeline", "", "build", "INFER003", "%s",
                "failed to link streammux → nvinfer");
      return false;
    }

    // Input probe on the nvinfer sink pad (frames entering inference).
    nvinfer_sink_pad_ = gst_element_get_static_pad(nvinfer_, "sink");
    if (nvinfer_sink_pad_) {
      input_probe_id_ = inference::install_input_probe(nvinfer_sink_pad_, metrics_.get());
      if (input_probe_id_ != 0) {
        LOG_INFO("pipeline", "input probe installed on nvinfer sink pad");
      } else {
        LOG_WARN("pipeline", "failed to install input probe on nvinfer sink pad");
      }
    }

    // Inference probe on the nvinfer src pad (frames leaving inference).
    nvinfer_src_pad_ = gst_element_get_static_pad(nvinfer_, "src");
    if (nvinfer_src_pad_) {
      infer_probe_id_ = inference::install_infer_probe(nvinfer_src_pad_, metrics_.get());
      if (infer_probe_id_ != 0) {
        LOG_INFO("pipeline", "infer probe installed on nvinfer src pad");
      } else {
        LOG_WARN("pipeline", "failed to install infer probe on nvinfer src pad");
      }
    }
    upstream = nvinfer_;
  } else {
    LOG_WARN("pipeline", "inference disabled — no detection path");
    if (!gst_element_link(upstream, sink_)) {
      LOG_ERROR("pipeline", "", "build", "PIPE005", "%s",
                "failed to link streammux → fakesink");
      return false;
    }
    upstream = nullptr;
  }

  // Tracker stage.
  if (upstream && tracker_config.enable) {
    if (tracker_config.ll_lib_file.empty() || tracker_config.ll_config_file.empty()) {
      LOG_ERROR("pipeline", "", "build", "TRK001", "%s",
                "tracker enabled but ll_lib_file/ll_config_file empty");
      return false;
    }
    tracker_ = gst_element_factory_make("nvtracker", "primary-tracker");
    if (!tracker_) {
      LOG_ERROR("pipeline", "", "build", "TRK002", "%s",
                "failed to create nvtracker element");
      return false;
    }
    g_object_set(G_OBJECT(tracker_),
                 "ll-lib-file", tracker_config.ll_lib_file.c_str(),
                 "ll-config-file", tracker_config.ll_config_file.c_str(),
                 "tracker-width", tracker_config.width,
                 "tracker-height", tracker_config.height,
                 "gpu-id", tracker_config.gpu_id,
                 nullptr);
    gst_bin_add(GST_BIN(pipeline_), tracker_);

    if (!gst_element_link_many(upstream, tracker_, sink_, nullptr)) {
      LOG_ERROR("pipeline", "", "build", "TRK003", "%s",
                "failed to link nvinfer → nvtracker → fakesink");
      return false;
    }
    upstream = tracker_;
    LOG_INFO("pipeline", "nvtracker enabled (lib=%s, config=%s)",
             tracker_config.ll_lib_file.c_str(), tracker_config.ll_config_file.c_str());
  } else if (upstream) {
    if (!gst_element_link(upstream, sink_)) {
      LOG_ERROR("pipeline", "", "build", "PIPE006", "%s",
                "failed to link nvinfer → fakesink");
      return false;
    }
  }

  // 6. Output probe on the last detection stage (tracker, or nvinfer if no
  // tracker).  Emits JSONL detections and updates output metrics.
  if (upstream) {
    GstPad* out_pad = tracker_ ? tracker_src_pad_ : nvinfer_src_pad_;
    if (!out_pad) {
      out_pad = gst_element_get_static_pad(upstream, "src");
      if (tracker_) {
        tracker_src_pad_ = out_pad;
      }
    }

    std::vector<std::string> class_names;
    if (!output_config.labels_file_path.empty()) {
      if (!inference::load_label_file(output_config.labels_file_path, class_names)) {
        LOG_WARN("pipeline", "labels file load failed, class names will be '?'");
      }
    }
    class_names_ = class_names;  // kept for LLM prompt building

    output_probe_id_ = inference::install_output_probe(
        out_pad, metrics_.get(), source_mgr_->stream_ids(), class_names,
        output_config.jsonl_path);
    if (output_probe_id_ != 0) {
      LOG_INFO("pipeline", "output probe installed on %s src pad",
               GST_OBJECT_NAME(upstream));
    } else {
      LOG_WARN("pipeline", "failed to install output probe");
    }

    // 6b. Event system (Stage 6): rule engine + JSONL + bounded keyframes.
    if (events_config.enable) {
      if (events_config.jsonl_path.empty()) {
        LOG_ERROR("pipeline", "", "build", "EVT010", "%s",
                  "events enabled but jsonl_path is empty");
        return false;
      }

      event_engine_ = std::make_unique<events::EventEngine>(events_config,
                                                            source_mgr_->stream_ids());
      event_writer_ = std::make_unique<events::EventWriter>();
      if (!event_writer_->open(events_config.jsonl_path, source_mgr_->stream_ids(),
                               class_names)) {
        LOG_ERROR("pipeline", "", "build", "EVT012", "%s",
                  "event JSONL init failed");
        return false;
      }

      if (!events_config.keyframe_dir.empty()) {
        keyframe_writer_ = std::make_unique<events::KeyframeWriter>();
        if (!keyframe_writer_->init(events_config.keyframe_dir,
                                    events_config.max_keyframes,
                                    events_config.jpeg_quality)) {
          LOG_WARN("pipeline", "keyframe writer init failed — keyframes disabled");
          keyframe_writer_.reset();
        }
      }

      // Stage 7: async cloud-analysis router (Qwen / DeepSeek).  Created
      // before the event probe so the probe can enqueue routed events.
      if (llm_config.enable) {
        llm_router_ = std::make_unique<llm::LlmRouter>();
        if (!llm_router_->init(llm_config, source_mgr_->stream_ids(),
                               class_names)) {
          LOG_ERROR("pipeline", "", "build", "LLM001", "%s",
                    "llm router init failed");
          llm_router_.reset();
        } else {
          llm_router_->start();
        }
      }

      event_probe_id_ = events::install_event_probe(
          out_pad, event_engine_.get(), keyframe_writer_.get(), event_writer_.get(),
          llm_router_.get(), source_mgr_->stream_ids(), class_names);
      if (event_probe_id_ == 0) {
        LOG_ERROR("pipeline", "", "build", "EVT013", "%s", "event probe install failed");
        return false;
      }
      LOG_INFO("pipeline", "event probe installed on %s src pad",
               GST_OBJECT_NAME(upstream));

      // Per-stream EOS → flush disappearance events for the ended stream.
      // GStreamer 1.20 has no GST_PAD_PROBE_TYPE_EOS; use downstream events
      // and filter for EOS.  Reinstalled after RTSP rebuilds (install_eos_probes).
      install_eos_probes();
    }
  }

  // 7. Install bus watch.
  loop_ = g_main_loop_new(nullptr, FALSE);
  GstBus* bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline_));
  bus_watch_id_ = gst_bus_add_watch(bus, on_bus_message, this);
  gst_object_unref(bus);

  // 8. Periodic metrics report (per-stream FPS + detection counts).
  if (output_config.fps_report_interval_sec > 0) {
    report_timer_id_ =
        g_timeout_add_seconds(output_config.fps_report_interval_sec,
                              on_periodic_report, this);
    LOG_INFO("pipeline", "periodic metrics report every %d s",
             output_config.fps_report_interval_sec);
  }

  // 8c. Stage 8: RTSP stall/reconnect watchdog (1s tick; drives the whole
  // reconnect state machine — stall detection, backoff deadlines, FPS verify).
  if (rtsp_config_.enable) {
    rtsp_watch_timer_id_ = g_timeout_add(1000, on_rtsp_watch, this);
    LOG_INFO("pipeline", "RTSP watchdog timer installed (1s tick)");
  }

  // 8d. Stage 9: deterministic scheduler driver (sample → policy → intervals).
  if (scheduler_config_.enable) {
    scheduler_ = std::make_unique<scheduler::SchedulerPolicy>(scheduler_config_);
    scheduler_intervals_.assign(stream_configs_.size(), 0);
    scheduler_timer_id_ = g_timeout_add_seconds(scheduler_config_.sample_interval_sec,
                                                on_scheduler_tick, this);
    LOG_INFO("pipeline",
             "scheduler enabled (tick=%ds cpu enter/exit=%.0f/%.0f%% temp "
             "enter/exit=%.0f/%.0f°C critical=%.0f/%.0f°C hold=%llds "
             "cooldown=%llds budget=%d/%llds)",
             scheduler_config_.sample_interval_sec,
             scheduler_config_.pressure_cpu_enter, scheduler_config_.pressure_cpu_exit,
             scheduler_config_.thermal_temp_enter, scheduler_config_.thermal_temp_exit,
             scheduler_config_.critical_temp_enter, scheduler_config_.critical_temp_exit,
             static_cast<long long>(scheduler_config_.min_hold_ms / 1000),
             static_cast<long long>(scheduler_config_.cooldown_ms / 1000),
             scheduler_config_.max_adjustments_per_window,
             static_cast<long long>(scheduler_config_.adjust_window_ms / 1000));
  }

  // 8b. Stage 7: periodic DeepSeek system-metrics analysis.  Low frequency
  // by design — the aggregated payload is small and the call is async.
  if (llm_router_ && llm_config.deepseek_interval_sec > 0) {
    llm_metrics_timer_id_ =
        g_timeout_add_seconds(llm_config.deepseek_interval_sec,
                              on_llm_metrics_report, this);
    LOG_INFO("pipeline", "DeepSeek metrics analysis every %d s",
             llm_config.deepseek_interval_sec);
  }

  // 8. Preroll.  Skipped for RTSP configs: live sources connect asynchronously
  // and a failed initial connect must not abort the process — the watchdog
  // drives reconnects instead.
  if (rtsp_config_.enable) {
    LOG_INFO("pipeline", "RTSP mode — skipping PAUSED preroll (live sources)");
  } else {
    GstStateChangeReturn ret = gst_element_set_state(pipeline_, GST_STATE_PAUSED);
    if (ret == GST_STATE_CHANGE_FAILURE) {
      LOG_ERROR("pipeline", "", "build", "STATE010", "%s", "failed to set pipeline to PAUSED");
      return false;
    }
  }

  LOG_INFO("pipeline", "pipeline built successfully with %d source(s)",
           source_mgr_->source_count());
  return true;
}

void Pipeline::run() {
  if (!pipeline_ || !loop_) {
    LOG_ERROR("pipeline", "", "run", "RUN010", "%s", "pipeline not built");
    return;
  }

  LOG_INFO("pipeline", "starting playback...");
  GstStateChangeReturn ret = gst_element_set_state(pipeline_, GST_STATE_PLAYING);
  if (ret == GST_STATE_CHANGE_FAILURE) {
    LOG_ERROR("pipeline", "", "run", "STATE011", "%s", "failed to set pipeline to PLAYING");
    return;
  }

  // Stage 8: every RTSP source starts a connect attempt; the watchdog
  // verifies frames arrive and then FPS, transitioning to RUNNING.
  if (rtsp_config_.enable) {
    const uint64_t now = mono_ms();
    for (size_t i = 0; i < rtsp_watch_.size(); ++i) {
      RtspWatch& w = rtsp_watch_[i];
      w.policy.mark_connect();
      w.connect_ms = now;
      w.state_since_ms = now;
      w.seen_state = w.policy.state();
      LOG_INFO("pipeline", "rtsp stream=%s → CONNECTING",
               source_mgr_ && i < source_mgr_->stream_ids().size()
                   ? source_mgr_->stream_ids()[i].c_str()
                   : "?");
    }
  }

  g_main_loop_run(loop_);

  LOG_INFO("pipeline", "stopping pipeline...");
  gst_element_set_state(pipeline_, GST_STATE_NULL);
  LOG_INFO("pipeline", "pipeline stopped");
}

void Pipeline::quit() {
  if (loop_) g_main_loop_quit(loop_);
}

void Pipeline::print_stats() const {
  if (source_mgr_) {
    source_mgr_->print_stats();
  }
  if (metrics_) {
    const auto summaries = metrics_->snapshot();
    std::printf("  %-10s %12s %12s %12s %10s %10s %12s %12s %12s\n",
                "stream", "in_frames", "infer_frames", "out_frames",
                "detections", "obj/frame", "in_fps", "infer_fps", "out_fps");
    for (const auto& s : summaries) {
      std::printf("  %-10s %12llu %12llu %12llu %10llu %10.2f %12.2f %12.2f %12.2f\n",
                  s.stream_id.c_str(),
                  static_cast<unsigned long long>(s.input_frames),
                  static_cast<unsigned long long>(s.infer_frames),
                  static_cast<unsigned long long>(s.output_frames),
                  static_cast<unsigned long long>(s.total_detections),
                  s.avg_detections_per_frame, s.avg_input_fps, s.avg_infer_fps,
                  s.avg_output_fps);
    }
  }
  if (event_engine_) {
    const auto counts = event_engine_->stream_counters();
    const auto ids = source_mgr_->stream_ids();
    std::printf("  %-10s %12s %12s %12s %12s %12s\n",
                "stream", "appear", "disappear", "count_high", "count_exit",
                "zone_entry");
    for (size_t i = 0; i < counts.size() && i < ids.size(); ++i) {
      std::printf("  %-10s %12llu %12llu %12llu %12llu %12llu\n",
                  ids[i].c_str(),
                  static_cast<unsigned long long>(counts[i].appearance),
                  static_cast<unsigned long long>(counts[i].disappearance),
                  static_cast<unsigned long long>(counts[i].count_high),
                  static_cast<unsigned long long>(counts[i].count_exit),
                  static_cast<unsigned long long>(counts[i].zone_entry));
    }
  }
}

std::vector<metrics::MetricsRegistry::StreamSummary> Pipeline::metrics_snapshot() const {
  if (!metrics_) {
    return {};
  }
  return metrics_->snapshot();
}

void Pipeline::print_metrics_log() const {
  if (!metrics_) {
    return;
  }
  const auto summaries = metrics_->snapshot();
  for (const auto& s : summaries) {
    LOG_INFO("metrics", "stream=%s in=%llu (%.1f fps) infer=%llu (%.1f fps) "
             "out=%llu (%.1f fps) detections=%llu obj/frame=%.2f",
             s.stream_id.c_str(),
             static_cast<unsigned long long>(s.input_frames), s.avg_input_fps,
             static_cast<unsigned long long>(s.infer_frames), s.avg_infer_fps,
             static_cast<unsigned long long>(s.output_frames), s.avg_output_fps,
             static_cast<unsigned long long>(s.total_detections),
             s.avg_detections_per_frame);
  }
  if (event_engine_) {
    const auto counts = event_engine_->stream_counters();
    const auto ids = source_mgr_->stream_ids();
    for (size_t i = 0; i < counts.size() && i < ids.size(); ++i) {
      LOG_INFO("metrics", "events stream=%s appearance=%llu disappearance=%llu "
               "count_high=%llu count_exit=%llu zone_entry=%llu",
               ids[i].c_str(),
               static_cast<unsigned long long>(counts[i].appearance),
               static_cast<unsigned long long>(counts[i].disappearance),
               static_cast<unsigned long long>(counts[i].count_high),
               static_cast<unsigned long long>(counts[i].count_exit),
               static_cast<unsigned long long>(counts[i].zone_entry));
    }
  }
}

// ---- Event helpers -----------------------------------------------------------

void Pipeline::flush_stream_events(int stream_idx, uint64_t ts_ms) {
  if (!event_engine_ || !event_writer_) {
    return;
  }
  const auto events = event_engine_->flush_stream(stream_idx, ts_ms);
  for (const auto& e : events) {
    event_writer_->write(e, "");  // no keyframe at EOS/shutdown
  }
}

// ---- Periodic timer ----------------------------------------------------------

gboolean Pipeline::on_periodic_report(gpointer user_data) {
  auto* self = static_cast<Pipeline*>(user_data);
  self->print_metrics_log();
  self->log_rtsp_states();      // Stage 8: per-stream RTSP state report
  self->log_scheduler_report();  // Stage 9: scheduler state report
  return G_SOURCE_CONTINUE;
}

// Aggregate current per-stream metrics + event counters into a compact JSON
// string for the DeepSeek diagnosis prompt (no raw logs, no secrets).
std::string Pipeline::build_metrics_json() const {
  Json::Value root;
  const auto summaries = metrics_ ? metrics_->snapshot()
                                  : std::vector<metrics::MetricsRegistry::StreamSummary>{};
  Json::Value streams(Json::arrayValue);
  for (const auto& s : summaries) {
    Json::Value js;
    js["stream_id"] = s.stream_id;
    js["input_frames"] = Json::Value::UInt64(s.input_frames);
    js["infer_frames"] = Json::Value::UInt64(s.infer_frames);
    js["output_frames"] = Json::Value::UInt64(s.output_frames);
    js["detections"] = Json::Value::UInt64(s.total_detections);
    js["obj_per_frame"] = s.avg_detections_per_frame;
    js["input_fps"] = s.avg_input_fps;
    js["infer_fps"] = s.avg_infer_fps;
    js["output_fps"] = s.avg_output_fps;
    streams.append(js);
  }
  root["streams"] = streams;

  if (event_engine_) {
    const auto counts = event_engine_->stream_counters();
    const auto ids = source_mgr_ ? source_mgr_->stream_ids() : std::vector<std::string>{};
    Json::Value events(Json::arrayValue);
    for (size_t i = 0; i < counts.size() && i < ids.size(); ++i) {
      Json::Value je;
      je["stream_id"] = ids[i];
      je["appearance"] = Json::Value::UInt64(counts[i].appearance);
      je["disappearance"] = Json::Value::UInt64(counts[i].disappearance);
      je["count_high"] = Json::Value::UInt64(counts[i].count_high);
      je["count_exit"] = Json::Value::UInt64(counts[i].count_exit);
      je["zone_entry"] = Json::Value::UInt64(counts[i].zone_entry);
      events.append(je);
    }
    root["events"] = events;
  }

  if (llm_router_) {
    const auto st = llm_router_->stats();
    Json::Value js;
    js["enqueued"] = Json::Value::UInt64(st.enqueued);
    js["shed"] = Json::Value::UInt64(st.shed);
    js["sent"] = Json::Value::UInt64(st.sent);
    js["succeeded"] = Json::Value::UInt64(st.succeeded);
    js["failed"] = Json::Value::UInt64(st.failed);
    js["queued"] = Json::Value::UInt64(st.queued_now);
    root["llm"] = js;
  }

  Json::FastWriter writer;
  return writer.write(root);
}

gboolean Pipeline::on_llm_metrics_report(gpointer user_data) {
  auto* self = static_cast<Pipeline*>(user_data);
  if (!self->llm_router_) {
    return G_SOURCE_CONTINUE;
  }
  const std::string metrics_json = self->build_metrics_json();
  if (self->llm_router_->enqueue_metrics_analysis(metrics_json)) {
    LOG_INFO("pipeline", "enqueued DeepSeek metrics analysis");
  }
  return G_SOURCE_CONTINUE;
}

// ---- Per-stream EOS ----------------------------------------------------------

GstPadProbeReturn Pipeline::on_stream_eos(GstPad* pad, GstPadProbeInfo* info,
                                          gpointer user_data) {
  auto* self = static_cast<Pipeline*>(user_data);
  GstEvent* event = GST_PAD_PROBE_INFO_EVENT(info);
  if (!event || GST_EVENT_TYPE(event) != GST_EVENT_EOS) {
    return GST_PAD_PROBE_OK;  // only handle downstream EOS
  }
  for (size_t i = 0; i < self->eos_pads_.size(); ++i) {
    if (self->eos_pads_[i] == pad) {
      LOG_INFO("pipeline", "EOS for source %d — flushing disappearance events",
               static_cast<int>(i));
      self->flush_stream_events(static_cast<int>(i), now_ms());
      break;
    }
  }
  return GST_PAD_PROBE_OK;
}

// ---- Stage 8: RTSP reconnect driver ------------------------------------------

void Pipeline::install_eos_probes() {
  // Remove probes on pads still alive; pads destroyed by a teardown already
  // had their probes removed (probes die with the pad).
  for (size_t i = 0; i < eos_pads_.size() && i < eos_probe_ids_.size(); ++i) {
    if (eos_probe_ids_[i] != 0 && eos_pads_[i]) {
      gst_pad_remove_probe(eos_pads_[i], eos_probe_ids_[i]);
    }
  }
  eos_pads_.clear();
  eos_probe_ids_.clear();

  if (!event_engine_ || !source_mgr_) {
    return;
  }
  for (int i = 0; i < source_mgr_->source_count(); ++i) {
    GstPad* dpad = source_mgr_->decoder_src_pad(i);
    if (!dpad) {
      continue;
    }
    const guint pid = gst_pad_add_probe(dpad, GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
                                        on_stream_eos, this, nullptr);
    if (pid != 0) {
      eos_pads_.push_back(dpad);
      eos_probe_ids_.push_back(pid);
    }
  }
}

gboolean Pipeline::on_rtsp_watch(gpointer user_data) {
  auto* self = static_cast<Pipeline*>(user_data);
  self->tick_rtsp_watch();
  return G_SOURCE_CONTINUE;
}

void Pipeline::tick_rtsp_watch() {
  if (!rtsp_config_.enable || !source_mgr_) {
    return;
  }
  for (size_t i = 0; i < rtsp_watch_.size(); ++i) {
    RtspWatch& w = rtsp_watch_[i];
    if (i >= source_mgr_->stream_ids().size()) {
      break;
    }
    // stream_ids() returns a temporary vector; copy the element so `sid`
    // outlives this block (a reference would dangle after the full expression).
    const std::string sid = source_mgr_->stream_ids()[i];
    if (!source_mgr_->is_rtsp_source(static_cast<int>(i))) {
      continue;  // file sources are not watched
    }

    // Read the clock per stream: a rebuild of an earlier stream inside this
    // loop (do_reconnect below) can take tens of ms, so a single `now`
    // captured at tick start goes stale and `now - last` underflows when a
    // frame probe fires in between (observed: false DEGRADED for the stream
    // checked right after a rebuild, every time).
    const uint64_t now = mono_ms();

    switch (w.policy.state()) {
      case StreamState::kOffline:
        break;  // not started (startup marks CONNECTING)

      case StreamState::kConnecting: {
        const uint64_t frames = source_mgr_->frame_count(static_cast<int>(i));
        if (!w.verifying && frames > 0) {
          // First frames arrived — open the FPS verification window.
          w.verifying = true;
          w.verify_start_ms = now;
          w.verify_start_count = frames;
        }
        if (w.verifying) {
          const uint64_t elapsed = now - w.verify_start_ms;
          if (elapsed >= static_cast<uint64_t>(rtsp_config_.verify_sec) * 1000) {
            const double fps = (frames - w.verify_start_count) * 1000.0 / elapsed;
            if (fps >= rtsp_config_.min_fps) {
              w.policy.mark_running();
              w.verifying = false;
              LOG_INFO("pipeline", "rtsp stream=%s verified: %.1f fps ≥ %.1f → RUNNING",
                       sid.c_str(), fps, rtsp_config_.min_fps);
            } else {
              w.verifying = false;
              LOG_WARN("pipeline", "rtsp stream=%s FPS verify failed: %.1f fps < %.1f",
                       sid.c_str(), fps, rtsp_config_.min_fps);
              schedule_reconnect(i, "fps-verify");
            }
          }
        } else if (now - w.connect_ms >=
                   static_cast<uint64_t>(rtsp_config_.first_frame_timeout_sec) * 1000) {
          // Initial no-frames window is deliberately longer than the stall
          // window: a live stream connecting mid-GOP cannot produce its first
          // decoded frame until the next keyframe (8.33 s GOP measured on this
          // test environment), and tearing the session down before that only
          // restarts the same wait.
          LOG_WARN("pipeline", "rtsp stream=%s: no frames within %ds of connect",
                   sid.c_str(), rtsp_config_.first_frame_timeout_sec);
          schedule_reconnect(i, "no-frames");
        }
        break;
      }

      case StreamState::kRunning: {
        const uint64_t last = source_mgr_->last_frame_ts_ms(static_cast<int>(i));
        // `last > now` is a benign race with the frame probe (probe stored
        // its timestamp after `now` was captured); guard the unsigned
        // subtraction so a freshly-firing stream can never underflow into a
        // false stall.
        if (last != 0 && last <= now && now - last >
                             static_cast<uint64_t>(rtsp_config_.watch_timeout_sec) * 1000) {
          w.policy.mark_degraded();
          LOG_WARN("pipeline", "rtsp stream=%s: no frames for %ds — DEGRADED",
                   sid.c_str(), rtsp_config_.watch_timeout_sec);
          schedule_reconnect(i, "stall");
        }
        break;
      }

      case StreamState::kDegraded:
        break;  // transient, immediately followed by kReconnecting

      case StreamState::kReconnecting:
        if (now >= w.deadline_ms) {
          do_reconnect(i);
        }
        break;

      case StreamState::kFailed:
        break;  // retry budget exhausted — no more automatic attempts
    }

    // Track state age for the periodic report.
    if (w.policy.state() != w.seen_state) {
      w.seen_state = w.policy.state();
      w.state_since_ms = now;
    }
  }
}

void Pipeline::schedule_reconnect(size_t idx, const char* reason) {
  if (idx >= rtsp_watch_.size() || !source_mgr_) {
    return;
  }
  RtspWatch& w = rtsp_watch_[idx];
  const std::string sid = idx < source_mgr_->stream_ids().size()
                              ? source_mgr_->stream_ids()[idx]
                              : "?";
  const uint64_t now = mono_ms();

  // Coalesce failure signals.  A teardown/rebuild of this same stream posts
  // several bus ERRORs from the dying rtspsrc ("Internal data stream error",
  // "Could not write to resource"); counting each one as a separate failure
  // exhausts the retry budget on a single real failure.  While a reconnect is
  // already pending (or the stream is FAILED) ignore new signals.
  if (w.policy.state() == StreamState::kFailed) {
    return;
  }
  if (w.policy.state() == StreamState::kReconnecting && now < w.deadline_ms) {
    LOG_INFO("pipeline", "rtsp stream=%s: reconnect already pending for "
             "reason='%s' — ignoring failure signal '%s'",
             sid.c_str(), w.policy.last_reason(), reason);
    return;
  }

  const StreamState st = w.policy.mark_failure(reason);
  if (st == StreamState::kFailed) {
    LOG_ERROR("pipeline", sid.c_str(), "RECONNECTING", "RTSP006",
              "%d consecutive failures (reason='%s') — FAILED, automatic "
              "reconnect stopped",
              w.policy.consecutive_failures(), reason);
    return;
  }
  w.deadline_ms = mono_ms() + w.policy.backoff_ms();
  LOG_WARN("pipeline", "rtsp stream=%s failure='%s' failures=%d backoff=%lld ms → "
           "RECONNECTING at +%lld ms",
           sid.c_str(), reason, w.policy.consecutive_failures(),
           static_cast<long long>(w.policy.backoff_ms()),
           static_cast<long long>(w.deadline_ms - mono_ms()));
}

void Pipeline::do_reconnect(size_t idx) {
  if (idx >= rtsp_watch_.size() || !source_mgr_) {
    return;
  }
  RtspWatch& w = rtsp_watch_[idx];
  const std::string sid = idx < source_mgr_->stream_ids().size()
                              ? source_mgr_->stream_ids()[idx]
                              : "?";

  // Remove this stream's EOS probe before its pad is destroyed by the teardown.
  if (idx < eos_probe_ids_.size() && eos_probe_ids_[idx] != 0 &&
      idx < eos_pads_.size() && eos_pads_[idx]) {
    gst_pad_remove_probe(eos_pads_[idx], eos_probe_ids_[idx]);
    eos_probe_ids_[idx] = 0;
  }

  LOG_INFO("pipeline", "rtsp reconnect attempt %d for %s",
           w.policy.attempts_since_success() + 1, sid.c_str());
  if (!source_mgr_->rebuild_source(static_cast<int>(idx), pipeline_)) {
    LOG_ERROR("pipeline", sid.c_str(), "RECONNECTING", "RTSP010",
              "%s", "rebuild failed, source left disconnected");
    schedule_reconnect(idx, "rebuild-failed");
    return;
  }

  // Reinstall EOS probes on all live decoder pads (the rebuilt one included).
  install_eos_probes();

  w.policy.mark_connect();
  w.connect_ms = mono_ms();
  w.verifying = false;

  // Self-heal: if a failed startup state change reset the whole pipeline
  // (e.g. the RTSP server was down when we started), raise it back to PLAYING.
  GstState cur = GST_STATE_NULL;
  const GstStateChangeReturn qr = gst_element_get_state(pipeline_, &cur, nullptr, 0);
  if (qr == GST_STATE_CHANGE_FAILURE || cur != GST_STATE_PLAYING) {
    LOG_WARN("pipeline", "pipeline not PLAYING (state=%s, rc=%s) — re-raising to PLAYING",
             gst_element_state_get_name(cur),
             gst_element_state_change_return_get_name(qr));
    gst_element_set_state(pipeline_, GST_STATE_PLAYING);
  }
}

void Pipeline::log_rtsp_states() const {
  if (!rtsp_config_.enable || rtsp_watch_.empty() || !source_mgr_) {
    return;
  }
  const uint64_t now = mono_ms();
  const auto ids = source_mgr_->stream_ids();
  for (size_t i = 0; i < rtsp_watch_.size(); ++i) {
    if (i >= ids.size()) {
      break;
    }
    const RtspWatch& w = rtsp_watch_[i];
    if (!source_mgr_->is_rtsp_source(static_cast<int>(i))) {
      continue;
    }
    LOG_INFO("pipeline", "rtsp stream=%s state=%s age=%llus reconnects=%lld "
             "failures=%d last_reason=%s frames=%llu",
             ids[i].c_str(), stream_state_str(w.policy.state()),
             static_cast<unsigned long long>((now - w.state_since_ms) / 1000),
             static_cast<long long>(w.policy.total_reconnects()),
             w.policy.consecutive_failures(), w.policy.last_reason(),
             static_cast<unsigned long long>(
                 source_mgr_->frame_count(static_cast<int>(i))));
  }
}

// ---- Stage 9: scheduler driver -------------------------------------------------

gboolean Pipeline::on_scheduler_tick(gpointer user_data) {
  auto* self = static_cast<Pipeline*>(user_data);
  self->tick_scheduler();
  return G_SOURCE_CONTINUE;
}

namespace {
int tier_interval(const scheduler::PolicyTable& t, StreamPriority p) {
  switch (p) {
    case StreamPriority::kHigh:   return t.high;
    case StreamPriority::kNormal: return t.normal;
    case StreamPriority::kLow:    return t.low;
  }
  return 0;
}
}  // namespace

void Pipeline::tick_scheduler() {
  if (!scheduler_) {
    return;
  }
  scheduler_last_sample_ = sys_sampler_.sample();
  if (scheduler_->update(scheduler_last_sample_, mono_ms())) {
    const auto& t = scheduler_->table();
    LOG_INFO("scheduler",
             "state=%s table=[high=%d normal=%d low=%d] cpu=%.1f%% mem=%.1f%% "
             "temp=%.1f°C (%s)",
             scheduler_state_str(scheduler_->state()), t.high, t.normal, t.low,
             scheduler_last_sample_.cpu_pct, scheduler_last_sample_.mem_pct,
             scheduler_last_sample_.temp_c, sys_sampler_.last_temp_zone().c_str());
    apply_scheduler_intervals();
  }
}

void Pipeline::apply_scheduler_intervals() {
  if (!scheduler_ || !source_mgr_) {
    return;
  }
  const auto& t = scheduler_->table();
  for (size_t i = 0; i < stream_configs_.size() && i < scheduler_intervals_.size();
       ++i) {
    const int interval = tier_interval(t, stream_configs_[i].priority);
    if (scheduler_intervals_[i] != interval) {
      LOG_INFO("scheduler", "stream=%s priority=%s interval %d → %d",
               stream_configs_[i].id.c_str(), priority_str(stream_configs_[i].priority),
               scheduler_intervals_[i], interval);
      source_mgr_->set_infer_interval(static_cast<int>(i), interval);
      scheduler_intervals_[i] = interval;
    }
  }
}

void Pipeline::log_scheduler_report() const {
  if (!scheduler_) {
    return;
  }
  const auto& t = scheduler_->table();
  LOG_INFO("metrics",
           "scheduler state=%s table=[%d %d %d] cpu=%.1f%% mem=%.1f%% "
           "temp=%.1f°C adjustments=%d/%d recovery_step=%d",
           scheduler_state_str(scheduler_->state()), t.high, t.normal, t.low,
           scheduler_last_sample_.cpu_pct, scheduler_last_sample_.mem_pct,
           scheduler_last_sample_.temp_c, scheduler_->adjustments_in_window(),
           scheduler_config_.max_adjustments_per_window, scheduler_->recovery_step());
}

int Pipeline::stream_index_from_object(GstObject* obj) const {
  if (!obj) {
    return -1;
  }
  const auto ids = source_mgr_ ? source_mgr_->stream_ids() : std::vector<std::string>{};
  GstObject* cur = static_cast<GstObject*>(gst_object_ref(obj));
  for (;;) {
    const gchar* name = GST_OBJECT_NAME(cur);
    if (name && g_str_has_prefix(name, "src-")) {
      // "src-<id>-<role>" → <id>
      const char* rest = name + 4;
      const char* dash = std::strchr(rest, '-');
      const std::string id = dash ? std::string(rest, dash) : std::string(rest);
      for (size_t i = 0; i < ids.size(); ++i) {
        if (ids[i] == id) {
          gst_object_unref(cur);
          return static_cast<int>(i);
        }
      }
      gst_object_unref(cur);
      return -1;  // our element but unknown id (should not happen)
    }
    GstObject* parent = gst_object_get_parent(cur);
    gst_object_unref(cur);
    if (!parent) {
      return -1;
    }
    if (GST_IS_PIPELINE(parent)) {
      gst_object_unref(parent);
      return -1;
    }
    cur = parent;
  }
}

// ---- GStreamer callbacks ----------------------------------------------------

gboolean Pipeline::on_bus_message(GstBus* /*bus*/, GstMessage* msg, gpointer user_data) {
  Pipeline* self = static_cast<Pipeline*>(user_data);

  switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_EOS:
      LOG_INFO("pipeline", "EOS received, quitting main loop");
      g_main_loop_quit(self->loop_);
      break;

    case GST_MESSAGE_ERROR: {
      gchar* debug = nullptr;
      GError* error = nullptr;
      gst_message_parse_error(msg, &error, &debug);

      // Stage 8 fault isolation: errors from a source chain (rtspsrc /
      // parser / decoder — anything under our "src-<id>-*" elements) are
      // stream-level and trigger a per-stream reconnect; the other streams
      // keep running.  Anything else is fatal.
      const int stream_idx = self->stream_index_from_object(GST_MESSAGE_SRC(msg));
      if (stream_idx >= 0 && stream_idx < static_cast<int>(self->rtsp_watch_.size()) &&
          self->source_mgr_ && self->source_mgr_->is_rtsp_source(stream_idx)) {
        // Ignore errors from elements that were replaced by a rebuild: the
        // dying rtspsrc posts "Internal data stream error" / "Could not write
        // to resource" while being torn down, and those messages reach the
        // bus AFTER the fresh chain is already up.  Counting them as new
        // failures would exhaust the retry budget on a healthy stream.
        if (!self->source_mgr_->is_current_chain_element(
                stream_idx, GST_MESSAGE_SRC(msg))) {
          LOG_INFO("pipeline", "stale error from replaced %s element (%s) — "
                   "ignoring (chain already rebuilt)",
                   GST_OBJECT_NAME(msg->src), error->message);
          g_free(debug);
          g_error_free(error);
          return TRUE;
        }
        LOG_WARN("pipeline", "stream-level error from %s: %s — scheduling reconnect",
                 GST_OBJECT_NAME(msg->src), error->message);
        self->schedule_reconnect(static_cast<size_t>(stream_idx), "gst-error");
      } else {
        LOG_ERROR("pipeline", "", "running", "GST_ERROR",
                  "fatal error from %s: %s", GST_OBJECT_NAME(msg->src), error->message);
        if (debug) {
          LOG_ERROR("pipeline", "", "running", "GST_ERROR", "debug: %s", debug);
        }
        g_main_loop_quit(self->loop_);
      }
      g_free(debug);
      g_error_free(error);
      break;
    }

    case GST_MESSAGE_WARNING: {
      gchar* debug = nullptr;
      GError* error = nullptr;
      gst_message_parse_warning(msg, &error, &debug);
      LOG_WARN("pipeline", "from %s: %s", GST_OBJECT_NAME(msg->src), error->message);
      g_free(debug);
      g_error_free(error);
      break;
    }

    case GST_MESSAGE_STATE_CHANGED: {
      if (GST_MESSAGE_SRC(msg) == GST_OBJECT(self->pipeline_)) {
        GstState old_state, new_state, pending;
        gst_message_parse_state_changed(msg, &old_state, &new_state, &pending);
        LOG_INFO("pipeline", "state: %s → %s (pending=%s)",
                 gst_element_state_get_name(old_state),
                 gst_element_state_get_name(new_state),
                 gst_element_state_get_name(pending));
      }
      break;
    }

    default:
      break;
  }

  return TRUE;
}

// ---- Helpers ----------------------------------------------------------------

void Pipeline::release_resources() {
  if (report_timer_id_ != 0) {
    g_source_remove(report_timer_id_);
    report_timer_id_ = 0;
  }
  if (llm_metrics_timer_id_ != 0) {
    g_source_remove(llm_metrics_timer_id_);
    llm_metrics_timer_id_ = 0;
  }
  if (rtsp_watch_timer_id_ != 0) {
    g_source_remove(rtsp_watch_timer_id_);
    rtsp_watch_timer_id_ = 0;
  }
  if (scheduler_timer_id_ != 0) {
    g_source_remove(scheduler_timer_id_);
    scheduler_timer_id_ = 0;
  }
  // Stop the LLM worker threads before destroying the event engine — the
  // router may still be draining the queue.
  if (llm_router_) {
    llm_router_->stop();
  }
  if (output_probe_id_ != 0 && (tracker_src_pad_ || nvinfer_src_pad_)) {
    GstPad* pad = tracker_src_pad_ ? tracker_src_pad_ : nvinfer_src_pad_;
    gst_pad_remove_probe(pad, output_probe_id_);
    output_probe_id_ = 0;
  }
  if (event_probe_id_ != 0 && (tracker_src_pad_ || nvinfer_src_pad_)) {
    GstPad* pad = tracker_src_pad_ ? tracker_src_pad_ : nvinfer_src_pad_;
    gst_pad_remove_probe(pad, event_probe_id_);
    event_probe_id_ = 0;
  }
  for (size_t i = 0; i < eos_pads_.size() && i < eos_probe_ids_.size(); ++i) {
    if (eos_probe_ids_[i] != 0) {
      gst_pad_remove_probe(eos_pads_[i], eos_probe_ids_[i]);
    }
  }
  eos_pads_.clear();
  eos_probe_ids_.clear();

  // Flush remaining disappearance events (Ctrl-C mid-run).
  if (source_mgr_ && event_engine_ && event_writer_) {
    const uint64_t ts = now_ms();
    for (int i = 0; i < source_mgr_->source_count(); ++i) {
      flush_stream_events(i, ts);
    }
    event_writer_->flush();
  }
  if (infer_probe_id_ != 0 && nvinfer_src_pad_) {
    gst_pad_remove_probe(nvinfer_src_pad_, infer_probe_id_);
    infer_probe_id_ = 0;
  }
  if (input_probe_id_ != 0 && nvinfer_sink_pad_) {
    gst_pad_remove_probe(nvinfer_sink_pad_, input_probe_id_);
    input_probe_id_ = 0;
  }
  if (tracker_src_pad_) {
    gst_object_unref(tracker_src_pad_);
    tracker_src_pad_ = nullptr;
  }
  if (nvinfer_src_pad_) {
    gst_object_unref(nvinfer_src_pad_);
    nvinfer_src_pad_ = nullptr;
  }
  if (nvinfer_sink_pad_) {
    gst_object_unref(nvinfer_sink_pad_);
    nvinfer_sink_pad_ = nullptr;
  }
  if (pipeline_) {
    gst_element_set_state(pipeline_, GST_STATE_NULL);
    gst_object_unref(GST_OBJECT(pipeline_));
    pipeline_ = nullptr;
  }
  sink_ = nullptr;
  nvinfer_ = nullptr;
  tracker_ = nullptr;
  source_mgr_.reset();
  metrics_.reset();
  keyframe_writer_.reset();
  event_writer_.reset();
  event_engine_.reset();
  llm_router_.reset();
  scheduler_.reset();

  if (loop_) {
    g_main_loop_unref(loop_);
    loop_ = nullptr;
  }
  bus_watch_id_ = 0;
}

}  // namespace pipeline
}  // namespace jetedge
