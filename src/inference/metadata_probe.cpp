// MetadataProbe implementation — per-stream input counting and JSONL output.

#include "jetedge/inference/metadata_probe.h"

#include <ctime>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <mutex>

#include <gstnvdsmeta.h>
#include <nvdsmeta.h>

#include "jetedge/common/logging.h"

namespace jetedge {
namespace inference {

namespace {

// ---- Shared probe context ---------------------------------------------------

struct OutputProbeContext {
  metrics::MetricsRegistry* metrics = nullptr;
  std::vector<std::string> stream_ids;
  std::vector<std::string> class_names;
  std::ofstream jsonl;
  std::mutex write_mu;
  uint64_t detections_written = 0;
};

uint64_t now_ms() {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000ULL +
         static_cast<uint64_t>(ts.tv_nsec / 1000000);
}

const char* class_name(const std::vector<std::string>& names, int class_id) {
  if (class_id >= 0 && class_id < static_cast<int>(names.size())) {
    return names[static_cast<size_t>(class_id)].c_str();
  }
  return "?";
}

// ---- Counting probes (nvinfer sink / nvinfer src / nvtracker src) -----------

template <typename Fn>
GstPadProbeReturn on_counting_probe(GstPad* /*pad*/, GstPadProbeInfo* info,
                                    gpointer user_data, Fn&& fn) {
  GstBuffer* buf = GST_PAD_PROBE_INFO_BUFFER(info);
  if (!buf) {
    return GST_PAD_PROBE_OK;
  }
  NvDsBatchMeta* batch_meta = gst_buffer_get_nvds_batch_meta(buf);
  if (!batch_meta) {
    return GST_PAD_PROBE_OK;
  }
  for (NvDsFrameMetaList* frame_item = batch_meta->frame_meta_list; frame_item;
       frame_item = frame_item->next) {
    NvDsFrameMeta* frame_meta = static_cast<NvDsFrameMeta*>(frame_item->data);
    if (!frame_meta) {
      continue;
    }
    fn(static_cast<int>(frame_meta->pad_index), frame_meta->frame_num);
  }
  return GST_PAD_PROBE_OK;
}

GstPadProbeReturn on_input_probe(GstPad* pad, GstPadProbeInfo* info,
                                 gpointer user_data) {
  auto* metrics = static_cast<metrics::MetricsRegistry*>(user_data);
  return on_counting_probe(pad, info, user_data,
                           [metrics](int idx, uint64_t frame_num) {
    metrics->on_input_frame(idx);
    metrics->on_latency_begin(idx, frame_num);
  });
}

GstPadProbeReturn on_infer_probe(GstPad* pad, GstPadProbeInfo* info,
                                 gpointer user_data) {
  auto* metrics = static_cast<metrics::MetricsRegistry*>(user_data);
  return on_counting_probe(pad, info, user_data,
                           [metrics](int idx, uint64_t /*frame_num*/) {
    metrics->on_infer_frame(idx);
  });
}

// ---- Output probe (nvtracker src pad) ---------------------------------------

GstPadProbeReturn on_output_probe(GstPad* /*pad*/, GstPadProbeInfo* info,
                                  gpointer user_data) {
  auto* ctx = static_cast<OutputProbeContext*>(user_data);
  GstBuffer* buf = GST_PAD_PROBE_INFO_BUFFER(info);
  if (!buf) {
    return GST_PAD_PROBE_OK;
  }
  NvDsBatchMeta* batch_meta = gst_buffer_get_nvds_batch_meta(buf);
  if (!batch_meta) {
    return GST_PAD_PROBE_OK;
  }

  const uint64_t ts_ms = now_ms();
  std::lock_guard<std::mutex> lock(ctx->write_mu);  // protects jsonl + counters

  for (NvDsFrameMetaList* frame_item = batch_meta->frame_meta_list; frame_item;
       frame_item = frame_item->next) {
    NvDsFrameMeta* frame_meta = static_cast<NvDsFrameMeta*>(frame_item->data);
    if (!frame_meta) {
      continue;
    }

    const int stream_idx = static_cast<int>(frame_meta->pad_index);
    std::string stream_id;
    if (stream_idx >= 0 && stream_idx < static_cast<int>(ctx->stream_ids.size())) {
      stream_id = ctx->stream_ids[static_cast<size_t>(stream_idx)];
    } else {
      stream_id = "unknown";
    }

    int obj_count = 0;
    for (NvDsObjectMetaList* obj_item = frame_meta->obj_meta_list; obj_item;
         obj_item = obj_item->next) {
      NvDsObjectMeta* obj = static_cast<NvDsObjectMeta*>(obj_item->data);
      if (!obj) {
        continue;
      }
      ++obj_count;

      // One JSONL line per detection.
      char line[512];
      std::snprintf(line, sizeof(line),
                    "{\"ts_ms\":%llu,\"stream_id\":\"%s\",\"frame_num\":%llu,"
                    "\"track_id\":%llu,\"class_id\":%d,\"class\":\"%s\","
                    "\"confidence\":%.4f,\"bbox\":[%.2f,%.2f,%.2f,%.2f]}\n",
                    static_cast<unsigned long long>(ts_ms), stream_id.c_str(),
                    static_cast<unsigned long long>(frame_meta->frame_num),
                    static_cast<unsigned long long>(obj->object_id),
                    static_cast<int>(obj->class_id),
                    class_name(ctx->class_names, static_cast<int>(obj->class_id)),
                    static_cast<double>(obj->confidence),
                    static_cast<double>(obj->rect_params.left),
                    static_cast<double>(obj->rect_params.top),
                    static_cast<double>(obj->rect_params.width),
                    static_cast<double>(obj->rect_params.height));
      if (ctx->jsonl.is_open()) {
        ctx->jsonl.write(line, static_cast<std::streamsize>(std::strlen(line)));
      }
      ++ctx->detections_written;
    }

    if (ctx->metrics) {
      ctx->metrics->on_output_frame(stream_idx, obj_count);
      ctx->metrics->on_latency_end(stream_idx, frame_meta->frame_num);
    }
  }
  return GST_PAD_PROBE_OK;
}

void on_output_probe_destroyed(gpointer user_data) {
  auto* ctx = static_cast<OutputProbeContext*>(user_data);
  if (ctx && ctx->jsonl.is_open()) {
    ctx->jsonl.flush();
    ctx->jsonl.close();
  }
  delete ctx;
}

}  // namespace

bool load_label_file(const std::string& path, std::vector<std::string>& names_out) {
  std::ifstream in(path);
  if (!in.is_open()) {
    LOG_ERROR("inference", "", "load_labels", "LABEL001", "cannot open %s", path.c_str());
    return false;
  }
  names_out.clear();
  std::string line;
  int line_index = 0;
  while (std::getline(in, line)) {
    if (line.empty()) {
      continue;
    }
    // Labelfile formats:
    //   "<id> <name>"   — DeepStream standard (ids may be sparse), e.g. "2 car"
    //   "<name>"        — plain list; the 0-based line index is the id
    const size_t space = line.find(' ');
    int id = line_index;
    std::string name = line;
    if (space != std::string::npos && space > 0 && line[0] >= '0' && line[0] <= '9') {
      id = std::atoi(line.substr(0, space).c_str());
      name = line.substr(space + 1);
    }
    if (id >= 0) {
      if (static_cast<size_t>(id) >= names_out.size()) {
        names_out.resize(static_cast<size_t>(id) + 1);
      }
      names_out[static_cast<size_t>(id)] = name;
    }
    ++line_index;
  }
  return true;
}

unsigned long install_input_probe(GstPad* pad, metrics::MetricsRegistry* metrics) {
  if (!pad || !metrics) {
    return 0;
  }
  return gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, on_input_probe,
                           metrics, nullptr);
}

unsigned long install_infer_probe(GstPad* pad, metrics::MetricsRegistry* metrics) {
  if (!pad || !metrics) {
    return 0;
  }
  return gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, on_infer_probe,
                           metrics, nullptr);
}

unsigned long install_output_probe(GstPad* pad,
                                   metrics::MetricsRegistry* metrics,
                                   const std::vector<std::string>& stream_ids,
                                   const std::vector<std::string>& class_names,
                                   const std::string& jsonl_path) {
  if (!pad) {
    return 0;
  }
  auto* ctx = new OutputProbeContext();
  ctx->metrics = metrics;
  ctx->stream_ids = stream_ids;
  ctx->class_names = class_names;
  if (!jsonl_path.empty()) {
    ctx->jsonl.open(jsonl_path, std::ios::out | std::ios::trunc);
    if (!ctx->jsonl.is_open()) {
      LOG_ERROR("inference", "", "install_output_probe", "JSONL001",
                "cannot open JSONL output %s (continuing without file output)",
                jsonl_path.c_str());
    } else {
      LOG_INFO("inference", "JSONL output: %s", jsonl_path.c_str());
    }
  }
  return gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, on_output_probe,
                           ctx, on_output_probe_destroyed);
}

}  // namespace inference
}  // namespace jetedge
