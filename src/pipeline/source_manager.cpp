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
                          const MuxConfig& mux_config) {
  if (stream_configs.empty()) {
    LOG_ERROR("source_mgr", "", "build", "MGR001", "%s", "no streams configured");
    return false;
  }

  const int batch_size = static_cast<int>(stream_configs.size());
  LOG_INFO("source_mgr", "building %d source(s)", batch_size);

  // 1. Create and build each SourceBin.
  for (const auto& sc : stream_configs) {
    auto src = std::make_unique<SourceBin>(sc);
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

  g_object_set(G_OBJECT(streammux_),
               "batch-size",           batch_size,
               "width",                mux_config.output_width,
               "height",               mux_config.output_height,
               "batched-push-timeout", mux_config.batch_timeout_usec,
               "live-source",          FALSE,
               nullptr);

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
