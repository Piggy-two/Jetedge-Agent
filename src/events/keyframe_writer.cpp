// KeyframeWriter implementation — whole-frame JPEG capture via the official
// DeepStream object encoder.
//
// On Jetson, gst_buffer_map on an NVMM buffer returns an NvBufSurface* header
// rather than pixels (verified against deepstream-test4
// pgie_src_pad_buffer_probe), and the mux output surfaces mix pitch-linear
// and block-linear layouts inside one batch (measured on this device).  The
// nvds_obj_enc API (libnvds_batch_jpegenc) encodes any NVMM surface to JPEG
// on the GPU, so it is used instead of manual CPU pixel extraction.

#include "jetedge/events/keyframe_writer.h"

#include <cerrno>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <sys/stat.h>
#include <sys/types.h>

#include "jetedge/common/logging.h"

namespace jetedge {
namespace events {

namespace {

std::string sanitize_name(std::string s) {
  for (char& c : s) {
    if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_')) {
      c = '_';
    }
  }
  return s;
}

}  // namespace

KeyframeWriter::~KeyframeWriter() {
  std::lock_guard<std::mutex> lock(mu_);
  if (enc_ctx_) {
    nvds_obj_enc_destroy_context(enc_ctx_);
    enc_ctx_ = nullptr;
  }
}

bool KeyframeWriter::init(const std::string& dir, int max_total, int quality) {
  std::lock_guard<std::mutex> lock(mu_);
  if (dir.empty()) {
    LOG_ERROR("events", "", "init", "KFW001", "%s", "keyframe dir is empty");
    return false;
  }
  if (mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) {
    LOG_ERROR("events", "", "init", "KFW002", "cannot create keyframe dir %s",
              dir.c_str());
    return false;
  }
  // GPU ID 0 — same convention as deepstream-test4.
  enc_ctx_ = nvds_obj_enc_create_context(0);
  if (!enc_ctx_) {
    LOG_ERROR("events", "", "init", "KFW018", "%s",
              "nvds_obj_enc_create_context failed — keyframes disabled");
    return false;
  }
  dir_ = dir;
  max_total_ = max_total < 0 ? 0 : max_total;
  quality_ = quality < 1 ? 1 : (quality > 100 ? 100 : quality);
  saved_ = 0;
  cap_warned_ = false;
  init_failed_logged_ = false;
  LOG_INFO("events", "keyframe writer ready (dir=%s max=%d quality=%d)",
           dir.c_str(), max_total_, quality_);
  return true;
}

bool KeyframeWriter::can_save() const {
  std::lock_guard<std::mutex> lock(mu_);
  return max_total_ <= 0 || saved_ < max_total_;
}

std::string KeyframeWriter::save(NvBufSurface* surf, NvDsFrameMeta* frame_meta,
                                 const std::string& stream_id,
                                 const std::string& event_name,
                                 uint64_t ts_ms) {
  if (!surf || !frame_meta) {
    return "";
  }

  {
    std::lock_guard<std::mutex> lock(mu_);
    if (!enc_ctx_) {
      if (!init_failed_logged_) {
        LOG_ERROR("events", "", "keyframe", "KFW018", "%s",
                  "encoder context missing — keyframes disabled");
        init_failed_logged_ = true;
      }
      return "";
    }
    if (max_total_ > 0 && saved_ >= max_total_) {
      if (!cap_warned_) {
        LOG_WARN("events", "keyframe cap %d reached, further keyframes skipped",
                 max_total_);
        cap_warned_ = true;
      }
      return "";
    }
  }

  // Encode the whole frame.  Same usage pattern as deepstream-image-meta-test:
  // isFrame=1, attachUsrMeta=TRUE, then read the JPEG back from
  // frame_meta->frame_user_meta_list after nvds_obj_enc_finish().
  NvDsObjEncUsrArgs args = {0};
  args.isFrame = 1;
  args.saveImg = FALSE;
  args.attachUsrMeta = TRUE;
  args.quality = quality_;
  if (!nvds_obj_enc_process(enc_ctx_, &args, surf, nullptr, frame_meta)) {
    LOG_ERROR("events", "", "keyframe", "KFW019",
              "nvds_obj_enc_process failed (stream=%s ts_ms=%llu)",
              stream_id.c_str(), static_cast<unsigned long long>(ts_ms));
    return "";
  }
  nvds_obj_enc_finish(enc_ctx_);

  // Copy the encoded JPEG out of the user meta (the encoder reuses its
  // internal buffers, so the bytes must be copied before returning).
  std::vector<uint8_t> jpg;
  for (NvDsUserMetaList* item = frame_meta->frame_user_meta_list; item;
       item = item->next) {
    NvDsUserMeta* usr = static_cast<NvDsUserMeta*>(item->data);
    if (!usr || usr->base_meta.meta_type != NVDS_CROP_IMAGE_META) {
      continue;
    }
    auto* enc = static_cast<NvDsObjEncOutParams*>(usr->user_meta_data);
    if (enc && enc->outBuffer && enc->outLen > 0) {
      jpg.assign(enc->outBuffer, enc->outBuffer + enc->outLen);
    }
    break;
  }
  if (jpg.empty()) {
    LOG_ERROR("events", "", "keyframe", "KFW021",
              "no NVDS_CROP_IMAGE_META after encode (stream=%s ts_ms=%llu)",
              stream_id.c_str(), static_cast<unsigned long long>(ts_ms));
    return "";
  }

  // ---- Write file ----------------------------------------------------------
  const std::string sid = sanitize_name(stream_id);
  const std::string evt = sanitize_name(event_name);
  const std::string name = sid + "_t" + std::to_string(ts_ms) + "_" + evt + ".jpg";
  const std::string path = dir_ + "/" + name;

  FILE* f = std::fopen(path.c_str(), "wb");
  if (!f) {
    LOG_ERROR("events", "", "keyframe", "KFW013", "cannot write keyframe %s",
              path.c_str());
    return "";
  }
  std::fwrite(jpg.data(), 1, jpg.size(), f);
  std::fclose(f);

  {
    std::lock_guard<std::mutex> lock(mu_);
    ++saved_;
  }
  LOG_INFO("events", "keyframe saved: %s (%zu bytes)", path.c_str(), jpg.size());
  return name;
}

int KeyframeWriter::saved() const {
  std::lock_guard<std::mutex> lock(mu_);
  return saved_;
}

}  // namespace events
}  // namespace jetedge
