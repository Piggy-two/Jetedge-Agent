// Pipeline implementation — decode → nvstreammux → nvinfer → nvtracker →
// fakesink (Stage 5).

#include "jetedge/pipeline/pipeline.h"

#include "jetedge/common/logging.h"
#include "jetedge/inference/metadata_probe.h"

namespace jetedge {
namespace pipeline {

Pipeline::~Pipeline() {
  release_resources();
}

bool Pipeline::build(const std::vector<StreamConfig>& stream_configs,
                     const MuxConfig& mux_config,
                     const inference::InferenceConfig& infer_config,
                     const TrackerConfig& tracker_config,
                     const OutputConfig& output_config) {
  if (stream_configs.empty()) {
    LOG_ERROR("pipeline", "", "build", "PIPE001", "%s", "no streams configured");
    return false;
  }

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
  if (!source_mgr_->build(pipeline_, stream_configs, mux_config)) {
    LOG_ERROR("pipeline", "", "build", "PIPE004", "%s", "source manager build failed");
    return false;
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

    output_probe_id_ = inference::install_output_probe(
        out_pad, metrics_.get(), source_mgr_->stream_ids(), class_names,
        output_config.jsonl_path);
    if (output_probe_id_ != 0) {
      LOG_INFO("pipeline", "output probe installed on %s src pad",
               GST_OBJECT_NAME(upstream));
    } else {
      LOG_WARN("pipeline", "failed to install output probe");
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

  // 8. Preroll.
  GstStateChangeReturn ret = gst_element_set_state(pipeline_, GST_STATE_PAUSED);
  if (ret == GST_STATE_CHANGE_FAILURE) {
    LOG_ERROR("pipeline", "", "build", "STATE010", "%s", "failed to set pipeline to PAUSED");
    return false;
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
}

// ---- Periodic timer ----------------------------------------------------------

gboolean Pipeline::on_periodic_report(gpointer user_data) {
  auto* self = static_cast<Pipeline*>(user_data);
  self->print_metrics_log();
  return G_SOURCE_CONTINUE;
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
      LOG_ERROR("pipeline", "", "running", "GST_ERROR",
                "from %s: %s", GST_OBJECT_NAME(msg->src), error->message);
      if (debug) {
        LOG_ERROR("pipeline", "", "running", "GST_ERROR", "debug: %s", debug);
      }
      g_free(debug);
      g_error_free(error);
      g_main_loop_quit(self->loop_);
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
  if (output_probe_id_ != 0 && (tracker_src_pad_ || nvinfer_src_pad_)) {
    GstPad* pad = tracker_src_pad_ ? tracker_src_pad_ : nvinfer_src_pad_;
    gst_pad_remove_probe(pad, output_probe_id_);
    output_probe_id_ = 0;
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

  if (loop_) {
    g_main_loop_unref(loop_);
    loop_ = nullptr;
  }
  bus_watch_id_ = 0;
}

}  // namespace pipeline
}  // namespace jetedge
