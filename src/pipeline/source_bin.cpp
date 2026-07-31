// SourceBin implementation.

#include "jetedge/pipeline/source_bin.h"

#include "jetedge/common/logging.h"

namespace jetedge {
namespace pipeline {

namespace {

bool ends_with(const std::string& str, const std::string& suffix) {
  if (suffix.size() > str.size()) return false;
  return std::equal(suffix.rbegin(), suffix.rend(), str.rbegin());
}

}  // namespace

SourceBin::SourceBin(const StreamConfig& config) : config_(config) {}

SourceBin::~SourceBin() {
  // Elements are owned by the parent pipeline bin — no explicit unref needed.
  // The probe will be detached when the pad is released.
  if (decoder_src_pad_) {
    gst_object_unref(decoder_src_pad_);
    decoder_src_pad_ = nullptr;
  }
}

bool SourceBin::build(GstElement* pipeline) {
  LOG_INFO("source_bin", "building source: id=%s uri=%s", config_.id.c_str(), config_.uri.c_str());

  // Create elements with stream-specific names.
  const std::string prefix = "src-" + config_.id;
  filesrc_ = gst_element_factory_make("filesrc", (prefix + "-filesrc").c_str());
  parser_  = gst_element_factory_make("h264parse", (prefix + "-parser").c_str());
  decoder_ = gst_element_factory_make("nvv4l2decoder", (prefix + "-decoder").c_str());

  if (!filesrc_ || !parser_ || !decoder_) {
    LOG_ERROR("source_bin", config_.id.c_str(), "build", "SRC001",
              "%s", "failed to create source elements");
    return false;
  }

  g_object_set(G_OBJECT(filesrc_), "location", config_.uri.c_str(), nullptr);

  // Check if we need a demuxer.
  const bool needs_demux = ends_with(config_.uri, ".mp4") ||
                           ends_with(config_.uri, ".mov") ||
                           ends_with(config_.uri, ".mkv") ||
                           ends_with(config_.uri, ".avi");

  // Add elements to pipeline.
  if (needs_demux) {
    demux_ = gst_element_factory_make("qtdemux", (prefix + "-demux").c_str());
    if (!demux_) {
      LOG_ERROR("source_bin", config_.id.c_str(), "build", "SRC002",
                "%s", "failed to create qtdemux");
      return false;
    }
    gst_bin_add_many(GST_BIN(pipeline), filesrc_, demux_, parser_, decoder_, nullptr);
  } else {
    gst_bin_add_many(GST_BIN(pipeline), filesrc_, parser_, decoder_, nullptr);
  }

  // ---- Link ---------------------------------------------------------------
  if (needs_demux) {
    // filesrc → qtdemux
    if (!gst_element_link(filesrc_, demux_)) {
      LOG_ERROR("source_bin", config_.id.c_str(), "build", "LINK010",
                "%s", "failed to link filesrc → qtdemux");
      return false;
    }
    // h264parse → decoder (static)
    if (!gst_element_link(parser_, decoder_)) {
      LOG_ERROR("source_bin", config_.id.c_str(), "build", "LINK011",
                "%s", "failed to link parser → decoder");
      return false;
    }
    // qtdemux dynamic pad → parser (via callback)
    g_signal_connect(demux_, "pad-added", G_CALLBACK(on_demux_pad_added), parser_);
  } else {
    // filesrc → h264parse → decoder (all static)
    if (!gst_element_link_many(filesrc_, parser_, decoder_, nullptr)) {
      LOG_ERROR("source_bin", config_.id.c_str(), "build", "LINK012",
                "%s", "failed to link filesrc → parser → decoder");
      return false;
    }
  }

  // ---- Install frame-counting probe on decoder src pad --------------------
  decoder_src_pad_ = gst_element_get_static_pad(decoder_, "src");
  if (!decoder_src_pad_) {
    LOG_ERROR("source_bin", config_.id.c_str(), "build", "PAD010",
              "%s", "failed to get decoder src pad");
    return false;
  }
  gst_pad_add_probe(decoder_src_pad_, GST_PAD_PROBE_TYPE_BUFFER,
                    on_frame_probe, this, nullptr);

  LOG_INFO("source_bin", "source %s built successfully (demux=%d)",
           config_.id.c_str(), needs_demux ? 1 : 0);
  return true;
}

// ---- Static callbacks -------------------------------------------------------

GstPadProbeReturn SourceBin::on_frame_probe(GstPad* /*pad*/, GstPadProbeInfo* info,
                                            gpointer user_data) {
  auto* self = static_cast<SourceBin*>(user_data);
  // Only count buffers (not events like EOS).
  if (info->type & GST_PAD_PROBE_TYPE_BUFFER) {
    self->frame_count_.fetch_add(1, std::memory_order_relaxed);
  }
  return GST_PAD_PROBE_OK;
}

void SourceBin::on_demux_pad_added(GstElement* /*src*/, GstPad* new_pad, gpointer user_data) {
  GstElement* parser = static_cast<GstElement*>(user_data);

  GstCaps* caps = gst_pad_get_current_caps(new_pad);
  if (!caps) caps = gst_pad_query_caps(new_pad, nullptr);
  if (!caps) return;

  GstStructure* structure = gst_caps_get_structure(caps, 0);
  const gchar* name = gst_structure_get_name(structure);

  if (name && g_str_has_prefix(name, "video/")) {
    GstPad* sink_pad = gst_element_get_static_pad(parser, "sink");
    if (sink_pad) {
      gst_pad_link(new_pad, sink_pad);
      gst_object_unref(sink_pad);
    }
  }

  gst_caps_unref(caps);
}

}  // namespace pipeline
}  // namespace jetedge
