// SourceManager implementation.

#include "jetedge/pipeline/source_manager.h"

#include <cstdio>

#include "jetedge/common/logging.h"

namespace jetedge {
namespace pipeline {

SourceManager::~SourceManager() {
  // SourceBins and streammux are owned by the pipeline bin.
}

bool SourceManager::build(GstElement* pipeline,
                          const std::vector<StreamConfig>& stream_configs,
                          const MuxConfig& mux_config,
                          const RtspConfig& rtsp_config) {
  if (stream_configs.empty()) {
    LOG_ERROR("source_mgr", "", "build", "MGR001", "%s", "no streams configured");
    return false;
  }

  const int batch_size = static_cast<int>(stream_configs.size());
  LOG_INFO("source_mgr", "building %d source(s)", batch_size);

  // 1. Create and build each SourceBin.
  configs_ = stream_configs;
  rtsp_config_ = rtsp_config;
  for (const auto& sc : stream_configs) {
    auto src = std::make_unique<SourceBin>(sc, rtsp_config_);
    if (!src->build(pipeline)) {
      LOG_ERROR("source_mgr", sc.id.c_str(), "build", "MGR002",
                "%s", "failed to build source bin, skipping this stream");
      continue;  // skip failed sources, keep others
    }
    sources_.push_back(std::move(src));
  }

  if (sources_.empty()) {
    LOG_ERROR("source_mgr", "", "build", "MGR003", "%s", "all sources failed to build");
    return false;
  }

  // 2. Create nvstreammux.
  streammux_ = gst_element_factory_make("nvstreammux", "stream-muxer");
  if (!streammux_) {
    LOG_ERROR("source_mgr", "", "build", "MGR004", "%s", "failed to create nvstreammux");
    return false;
  }

  // live-source=TRUE lets the mux push batches while a source is
  // disconnected/reconnecting (RTSP mode); file configs keep the old FALSE.
  const gboolean live = rtsp_config_.enable && rtsp_config_.live_source;
  g_object_set(G_OBJECT(streammux_),
               "batch-size",           batch_size,
               "width",                mux_config.output_width,
               "height",               mux_config.output_height,
               "batched-push-timeout", mux_config.batch_timeout_usec,
               "live-source",          live,
               nullptr);
  LOG_INFO("source_mgr", "nvstreammux live-source=%d", live);

  gst_bin_add(GST_BIN(pipeline), streammux_);

  // 3. Link each decoder src to a streammux request sink pad.
  for (int i = 0; i < static_cast<int>(sources_.size()); ++i) {
    gchar pad_name[32];
    g_snprintf(pad_name, sizeof(pad_name), "sink_%d", i);

    GstPad* sink_pad = gst_element_request_pad_simple(streammux_, pad_name);
    if (!sink_pad) {
      LOG_ERROR("source_mgr", sources_[i]->stream_id().c_str(), "build", "PAD020",
                "failed to request streammux pad '%s'", pad_name);
      return false;
    }

    GstPad* src_pad = sources_[i]->decoder_src_pad();
    if (gst_pad_link(src_pad, sink_pad) != GST_PAD_LINK_OK) {
      LOG_ERROR("source_mgr", sources_[i]->stream_id().c_str(), "build", "LINK020",
                "failed to link decoder src → streammux %s", pad_name);
      gst_object_unref(sink_pad);
      return false;
    }

    gst_object_unref(sink_pad);
    LOG_INFO("source_mgr", "linked %s → streammux %s",
             sources_[i]->stream_id().c_str(), pad_name);
  }

  LOG_INFO("source_mgr", "%d source(s) linked to nvstreammux (batch=%d)",
           static_cast<int>(sources_.size()), batch_size);
  return true;
}

std::vector<SourceManager::SourceStats> SourceManager::get_stats() const {
  std::vector<SourceStats> stats;
  stats.reserve(sources_.size());
  for (const auto& src : sources_) {
    stats.push_back({src->stream_id(), src->frame_count()});
  }
  return stats;
}

std::vector<std::string> SourceManager::stream_ids() const {
  std::vector<std::string> ids;
  ids.reserve(sources_.size());
  for (const auto& src : sources_) {
    ids.push_back(src->stream_id());
  }
  return ids;
}

GstPad* SourceManager::decoder_src_pad(int idx) const {
  if (idx < 0 || idx >= static_cast<int>(sources_.size())) {
    return nullptr;
  }
  return sources_[static_cast<size_t>(idx)]->decoder_src_pad();
}

uint64_t SourceManager::frame_count(int idx) const {
  if (idx < 0 || idx >= static_cast<int>(sources_.size())) {
    return 0;
  }
  return sources_[static_cast<size_t>(idx)]->frame_count();
}

uint64_t SourceManager::last_frame_ts_ms(int idx) const {
  if (idx < 0 || idx >= static_cast<int>(sources_.size())) {
    return 0;
  }
  return sources_[static_cast<size_t>(idx)]->last_frame_ts_ms();
}

bool SourceManager::is_rtsp_source(int idx) const {
  if (idx < 0 || idx >= static_cast<int>(sources_.size())) {
    return false;
  }
  return sources_[static_cast<size_t>(idx)]->is_rtsp();
}

bool SourceManager::is_current_chain_element(int idx, GstObject* obj) const {
  if (idx < 0 || idx >= static_cast<int>(sources_.size())) {
    return false;
  }
  return sources_[static_cast<size_t>(idx)]->is_chain_element(obj);
}

bool SourceManager::rebuild_source(int idx, GstElement* pipeline) {
  if (idx < 0 || idx >= static_cast<int>(sources_.size())) {
    LOG_ERROR("source_mgr", "", "rebuild", "MGR010", "invalid source index %d", idx);
    return false;
  }
  const size_t uidx = static_cast<size_t>(idx);
  if (!sources_[uidx]->is_rtsp()) {
    LOG_ERROR("source_mgr", sources_[uidx]->stream_id().c_str(), "rebuild", "MGR011",
              "%s", "only RTSP sources can be rebuilt");
    return false;
  }
  if (!configs_[uidx].type.empty() && configs_[uidx].id != sources_[uidx]->stream_id()) {
    LOG_ERROR("source_mgr", "", "rebuild", "MGR012", "config/source mismatch at %d", idx);
    return false;
  }

  // 1. Unlink decoder src → streammux sink pad, then release the request pad
  // so the mux forgets this source.  The pad name encodes the source_id, so
  // re-requesting "sink_<idx>" preserves the stream_id → pad index mapping.
  GstPad* src_pad = sources_[uidx]->decoder_src_pad();
  if (src_pad) {
    GstPad* sink_pad = gst_pad_get_peer(src_pad);
    if (sink_pad) {
      gst_pad_unlink(src_pad, sink_pad);
      gst_element_release_request_pad(streammux_, sink_pad);
      gst_object_unref(sink_pad);
    }
  }

  // 2. Tear down the old chain (elements → NULL, removed from the bin).
  sources_[uidx]->teardown(pipeline);

  // 3. Build a fresh chain.
  auto fresh = std::make_unique<SourceBin>(configs_[uidx], rtsp_config_);
  if (!fresh->build(pipeline)) {
    LOG_ERROR("source_mgr", configs_[uidx].id.c_str(), "rebuild", "MGR013",
              "%s", "rebuild failed, source left disconnected");
    fresh->teardown(pipeline);
    return false;
  }

  // 4. Re-request the same mux sink pad and link.
  gchar pad_name[32];
  g_snprintf(pad_name, sizeof(pad_name), "sink_%d", idx);
  GstPad* sink_pad = gst_element_request_pad_simple(streammux_, pad_name);
  if (!sink_pad) {
    LOG_ERROR("source_mgr", configs_[uidx].id.c_str(), "rebuild", "MGR014",
              "failed to re-request streammux pad '%s'", pad_name);
    fresh->teardown(pipeline);
    return false;
  }
  GstPad* dsrc = fresh->decoder_src_pad();
  if (gst_pad_link(dsrc, sink_pad) != GST_PAD_LINK_OK) {
    LOG_ERROR("source_mgr", configs_[uidx].id.c_str(), "rebuild", "MGR015",
              "failed to link rebuilt decoder → %s", pad_name);
    gst_object_unref(sink_pad);
    fresh->teardown(pipeline);
    return false;
  }
  gst_object_unref(sink_pad);

  // 5. Bring the new elements to the pipeline state (they were created NULL;
  // the parent bin is already PLAYING).
  fresh->sync_state_with_parent();

  sources_[uidx] = std::move(fresh);
  LOG_INFO("source_mgr", "%s rebuilt and relinked to %s",
           configs_[uidx].id.c_str(), pad_name);
  return true;
}

void SourceManager::print_stats() const {
  uint64_t total = 0;
  for (const auto& src : sources_) {
    const uint64_t cnt = src->frame_count();
    std::printf("  %-16s frames=%lu\n", src->stream_id().c_str(), static_cast<unsigned long>(cnt));
    total += cnt;
  }
  std::printf("  %-16s frames=%lu\n", "(total)", static_cast<unsigned long>(total));
}

}  // namespace pipeline
}  // namespace jetedge
