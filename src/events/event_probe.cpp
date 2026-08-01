// EventProbe implementation — see event_probe.h.

#include "jetedge/events/event_probe.h"

#include <ctime>
#include <vector>

#include <gstnvdsmeta.h>
#include <nvdsmeta.h>

#include "jetedge/common/logging.h"

namespace jetedge {
namespace events {

namespace {

uint64_t now_ms() {
  struct timespec ts;
  clock_gettime(CLOCK_REALTIME, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000ULL +
         static_cast<uint64_t>(ts.tv_nsec / 1000000);
}

struct EventProbeContext {
  EventEngine* engine = nullptr;
  KeyframeWriter* writer = nullptr;
  EventWriter* event_writer = nullptr;
  std::vector<std::string> stream_ids;
  std::vector<std::string> class_names;
};

const std::string& stream_id_at(const EventProbeContext* ctx, int idx) {
  if (idx >= 0 && idx < static_cast<int>(ctx->stream_ids.size())) {
    return ctx->stream_ids[static_cast<size_t>(idx)];
  }
  static const std::string kUnknown = "unknown";
  return kUnknown;
}

GstPadProbeReturn on_event_probe(GstPad* pad, GstPadProbeInfo* info,
                                 gpointer user_data) {
  auto* ctx = static_cast<EventProbeContext*>(user_data);
  GstBuffer* buf = GST_PAD_PROBE_INFO_BUFFER(info);
  if (!buf) {
    return GST_PAD_PROBE_OK;
  }
  NvDsBatchMeta* batch_meta = gst_buffer_get_nvds_batch_meta(buf);
  if (!batch_meta) {
    return GST_PAD_PROBE_OK;
  }
  if (!ctx->engine || !ctx->event_writer) {
    return GST_PAD_PROBE_OK;
  }

  // On Jetson an NVMM buffer maps to an NvBufSurface* header rather than
  // pixels (see deepstream-test4 pgie_src_pad_buffer_probe); the keyframe
  // encoder consumes the NvBufSurface directly.  Only map when keyframes are
  // still allowed.
  GstMapInfo map;
  NvBufSurface* surf = nullptr;
  if (ctx->writer && ctx->writer->can_save()) {
    if (gst_buffer_map(buf, &map, GST_MAP_READ)) {
      surf = reinterpret_cast<NvBufSurface*>(map.data);
    } else {
      LOG_ERROR("events", "", "keyframe", "KFW010", "%s",
                "gst_buffer_map failed for keyframe capture");
    }
  }

  const uint64_t ts_ms = now_ms();

  for (NvDsFrameMetaList* frame_item = batch_meta->frame_meta_list; frame_item;
       frame_item = frame_item->next) {
    NvDsFrameMeta* frame_meta = static_cast<NvDsFrameMeta*>(frame_item->data);
    if (!frame_meta) {
      continue;
    }
    const int stream_idx = static_cast<int>(frame_meta->pad_index);

    // Adapt object metadata → ObservedObject.
    std::vector<ObservedObject> objects;
    objects.reserve(16);
    for (NvDsObjectMetaList* obj_item = frame_meta->obj_meta_list; obj_item;
         obj_item = obj_item->next) {
      NvDsObjectMeta* obj = static_cast<NvDsObjectMeta*>(obj_item->data);
      if (!obj) {
        continue;
      }
      ObservedObject o;
      o.track_id = obj->object_id;
      o.class_id = static_cast<int>(obj->class_id);
      o.confidence = obj->confidence;
      o.left = obj->rect_params.left;
      o.top = obj->rect_params.top;
      o.width = obj->rect_params.width;
      o.height = obj->rect_params.height;
      objects.push_back(o);
    }

    const auto events = ctx->engine->process_frame(stream_idx, frame_meta->frame_num,
                                                   ts_ms, objects);
    for (const auto& e : events) {
      std::string keyframe_name;
      if (ctx->writer && surf) {
        // Save the whole frame that fired this event as a JPEG.
        keyframe_name = ctx->writer->save(
            surf, frame_meta, stream_id_at(ctx, stream_idx),
            event_type_str(e.type), ts_ms);
      }
      ctx->event_writer->write(e, keyframe_name);
    }
  }

  if (surf) {
    gst_buffer_unmap(buf, &map);
  }
  return GST_PAD_PROBE_OK;
}

void on_event_probe_destroyed(gpointer user_data) {
  auto* ctx = static_cast<EventProbeContext*>(user_data);
  delete ctx;
}

}  // namespace

unsigned long install_event_probe(GstPad* pad, EventEngine* engine,
                                  KeyframeWriter* writer, EventWriter* event_writer,
                                  const std::vector<std::string>& stream_ids,
                                  const std::vector<std::string>& class_names) {
  if (!pad || !engine || !event_writer) {
    return 0;
  }
  auto* ctx = new EventProbeContext();
  ctx->engine = engine;
  ctx->writer = writer;
  ctx->event_writer = event_writer;
  ctx->stream_ids = stream_ids;
  ctx->class_names = class_names;
  return gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, on_event_probe,
                           ctx, on_event_probe_destroyed);
}

}  // namespace events
}  // namespace jetedge
