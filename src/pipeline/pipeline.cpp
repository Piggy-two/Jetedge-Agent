// Pipeline implementation — decode → nvstreammux → nvinfer → fakesink.

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
                     const inference::InferenceConfig& infer_config) {
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

  // 4. Link streammux → [nvinfer] → fakesink.
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

    if (!gst_element_link_many(source_mgr_->streammux(), nvinfer_, sink_, nullptr)) {
      LOG_ERROR("pipeline", "", "build", "INFER003", "%s",
                "failed to link streammux → nvinfer → fakesink");
      return false;
    }

    nvinfer_src_pad_ = gst_element_get_static_pad(nvinfer_, "src");
    if (nvinfer_src_pad_) {
      metadata_probe_id_ = inference::install_metadata_probe(nvinfer_src_pad_);
      if (metadata_probe_id_ != 0) {
        LOG_INFO("pipeline", "metadata probe installed on nvinfer src pad");
      } else {
        LOG_WARN("pipeline", "failed to install metadata probe on nvinfer src pad");
      }
    }
  } else {
    if (!gst_element_link(source_mgr_->streammux(), sink_)) {
      LOG_ERROR("pipeline", "", "build", "PIPE005", "%s",
                "failed to link streammux → fakesink");
      return false;
    }
  }

  // 5. Install bus watch.
  loop_ = g_main_loop_new(nullptr, FALSE);
  GstBus* bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline_));
  bus_watch_id_ = gst_bus_add_watch(bus, on_bus_message, this);
  gst_object_unref(bus);

  // 6. Preroll.
  GstStateChangeReturn ret = gst_element_set_state(pipeline_, GST_STATE_PAUSED);
  if (ret == GST_STATE_CHANGE_FAILURE) {
    LOG_ERROR("pipeline", "", "build", "STATE010", "%s", "failed to set pipeline to PAUSED");
    return false;
  }

  LOG_INFO("pipeline", "pipeline built successfully with %d source(s)", source_mgr_->source_count());
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
  if (metadata_probe_id_ != 0 && nvinfer_src_pad_) {
    gst_pad_remove_probe(nvinfer_src_pad_, metadata_probe_id_);
    metadata_probe_id_ = 0;
  }
  if (nvinfer_src_pad_) {
    gst_object_unref(nvinfer_src_pad_);
    nvinfer_src_pad_ = nullptr;
  }
  if (pipeline_) {
    gst_element_set_state(pipeline_, GST_STATE_NULL);
    gst_object_unref(GST_OBJECT(pipeline_));
    pipeline_ = nullptr;
  }
  sink_ = nullptr;
  nvinfer_ = nullptr;
  source_mgr_.reset();

  if (loop_) {
    g_main_loop_unref(loop_);
    loop_ = nullptr;
  }
  bus_watch_id_ = 0;
}

}  // namespace pipeline
}  // namespace jetedge
