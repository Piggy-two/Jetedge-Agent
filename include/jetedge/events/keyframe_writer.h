// KeyframeWriter — bounded local keyframe cache (Stage 6).
//
// Saves one event-triggered frame as a JPEG file using the official DeepStream
// object encoder (nvds_obj_enc_process with isFrame=1, verified against
// deepstream-test4 / deepstream-image-meta-test on this device).  The encoder
// handles any NVMM surface layout (pitch / block-linear) internally via the
// batch JPEG encoder, so no manual NvBufSurface pixel extraction is needed.
// The total number of saved keyframes is capped by the configuration.

#pragma once

#include <mutex>
#include <string>

#include <gst/gst.h>
#include <gstnvdsmeta.h>
#include <nvbufsurface.h>
#include <nvds_obj_encode.h>

namespace jetedge {
namespace events {

class KeyframeWriter {
 public:
  KeyframeWriter() = default;
  ~KeyframeWriter();

  KeyframeWriter(const KeyframeWriter&) = delete;
  KeyframeWriter& operator=(const KeyframeWriter&) = delete;

  // dir: output directory (created if missing); max_total: global cap
  // (0 = unlimited); quality: JPEG quality 1..100 (clamped).
  bool init(const std::string& dir, int max_total, int quality);

  // True while a new keyframe is still allowed by the cap.
  bool can_save() const;

  // Encode the whole frame that `frame_meta` describes (from the NVMM batch
  // `surf`) as a JPEG and save it as
  // "<stream_id>_t<ts_ms>_<event_name>.jpg" under dir_.
  // Returns the file name (relative to dir_) or "" on failure.
  std::string save(NvBufSurface* surf, NvDsFrameMeta* frame_meta,
                   const std::string& stream_id, const std::string& event_name,
                   uint64_t ts_ms);

  int saved() const;

 private:
  mutable std::mutex mu_;
  NvDsObjEncCtxHandle enc_ctx_ = nullptr;
  std::string dir_;
  int max_total_ = 0;
  int quality_ = 85;
  int saved_ = 0;
  bool cap_warned_ = false;
  bool init_failed_logged_ = false;
};

}  // namespace events
}  // namespace jetedge
