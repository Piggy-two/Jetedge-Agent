// MetadataProbe — read NvDsBatchMeta from the nvinfer src pad and log
// per-frame detection counts (Stage 4 acceptance evidence).

#include "jetedge/inference/metadata_probe.h"

#include "jetedge/common/logging.h"

#include <gstnvdsmeta.h>
#include <nvdsmeta.h>

namespace jetedge {
namespace inference {

namespace {

GstPadProbeReturn on_infer_src_probe(GstPad* /*pad*/, GstPadProbeInfo* info,
                                     gpointer /*user_data*/) {
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

    // Count objects and find the highest-confidence one for logging.
    int obj_count = 0;
    NvDsObjectMeta* best_obj = nullptr;
    for (NvDsObjectMetaList* obj_item = frame_meta->obj_meta_list; obj_item;
         obj_item = obj_item->next) {
      NvDsObjectMeta* obj = static_cast<NvDsObjectMeta*>(obj_item->data);
      if (!obj) {
        continue;
      }
      ++obj_count;
      if (!best_obj || obj->confidence > best_obj->confidence) {
        best_obj = obj;
      }
    }

    if (obj_count > 0 && best_obj) {
      LOG_INFO("inference",
               "stream=%d frame=%u objects=%d top: class=%u conf=%.3f box=[%.0f,%.0f %.0fx%.0f]",
               frame_meta->pad_index, frame_meta->frame_num, obj_count,
               best_obj->class_id, best_obj->confidence,
               best_obj->rect_params.left, best_obj->rect_params.top,
               best_obj->rect_params.width, best_obj->rect_params.height);
    }
  }

  return GST_PAD_PROBE_OK;
}

}  // namespace

unsigned long install_metadata_probe(GstPad* pad) {
  if (!pad) {
    return 0;
  }
  return gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, on_infer_src_probe,
                           nullptr, nullptr);
}

}  // namespace inference
}  // namespace jetedge
