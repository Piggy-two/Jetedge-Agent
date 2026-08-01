// MetricsRegistry implementation (Stage 5).

#include "jetedge/metrics/metrics_registry.h"

#include <ctime>

#include "jetedge/common/logging.h"

namespace jetedge {
namespace metrics {

namespace {
constexpr uint64_t kWindowDurationNs = 2ULL * 1000 * 1000 * 1000;  // 2s window
}

uint64_t MetricsRegistry::now_ns() {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<uint64_t>(ts.tv_sec) * 1000000000ULL + ts.tv_nsec;
}

int MetricsRegistry::register_stream(const std::string& stream_id) {
  std::lock_guard<std::mutex> lock(mu_);
  PerStream ps;
  ps.stream_id = stream_id;
  const uint64_t t = now_ns();
  ps.run_start_ns = t;
  ps.window_start_ns = t;
  streams_.push_back(std::move(ps));
  return static_cast<int>(streams_.size()) - 1;
}

void MetricsRegistry::maybe_roll_window(PerStream* ps, uint64_t now) {
  const uint64_t elapsed = now - ps->window_start_ns;
  if (elapsed >= kWindowDurationNs) {
    const double secs = static_cast<double>(elapsed) / 1e9;
    ps->latest_input_fps  = static_cast<double>(ps->window_input) / secs;
    ps->latest_infer_fps  = static_cast<double>(ps->window_infer) / secs;
    ps->latest_output_fps = static_cast<double>(ps->window_output) / secs;
    ps->window_input = 0;
    ps->window_infer = 0;
    ps->window_output = 0;
    ps->window_start_ns = now;
  }
}

void MetricsRegistry::on_input_frame(int stream_idx) {
  if (stream_idx < 0 || stream_idx >= static_cast<int>(streams_.size())) {
    LOG_WARN("metrics", "on_input_frame: invalid stream index %d", stream_idx);
    return;
  }
  std::lock_guard<std::mutex> lock(mu_);
  PerStream* ps = &streams_[static_cast<size_t>(stream_idx)];
  ++ps->input_frames;
  ++ps->window_input;
  maybe_roll_window(ps, now_ns());
}

void MetricsRegistry::on_infer_frame(int stream_idx) {
  if (stream_idx < 0 || stream_idx >= static_cast<int>(streams_.size())) {
    LOG_WARN("metrics", "on_infer_frame: invalid stream index %d", stream_idx);
    return;
  }
  std::lock_guard<std::mutex> lock(mu_);
  PerStream* ps = &streams_[static_cast<size_t>(stream_idx)];
  ++ps->infer_frames;
  ++ps->window_infer;
  maybe_roll_window(ps, now_ns());
}

void MetricsRegistry::on_output_frame(int stream_idx, int obj_count) {
  if (stream_idx < 0 || stream_idx >= static_cast<int>(streams_.size())) {
    LOG_WARN("metrics", "on_output_frame: invalid stream index %d", stream_idx);
    return;
  }
  std::lock_guard<std::mutex> lock(mu_);
  PerStream* ps = &streams_[static_cast<size_t>(stream_idx)];
  ++ps->output_frames;
  if (obj_count > 0) {
    ++ps->frames_with_objects;
    ps->total_detections += static_cast<uint64_t>(obj_count);
  }
  ++ps->window_output;
  maybe_roll_window(ps, now_ns());
}

std::vector<MetricsRegistry::StreamSummary> MetricsRegistry::snapshot() const {
  std::lock_guard<std::mutex> lock(mu_);
  std::vector<StreamSummary> out;
  out.reserve(streams_.size());
  for (const auto& ps : streams_) {
    StreamSummary s;
    s.stream_id = ps.stream_id;
    s.input_frames = ps.input_frames;
    s.infer_frames = ps.infer_frames;
    s.output_frames = ps.output_frames;
    s.frames_with_objects = ps.frames_with_objects;
    s.total_detections = ps.total_detections;
    s.latest_input_fps = ps.latest_input_fps;
    s.latest_infer_fps = ps.latest_infer_fps;
    s.latest_output_fps = ps.latest_output_fps;
    if (ps.input_frames > 0) {
      const uint64_t elapsed_ns = now_ns() - ps.run_start_ns;
      double run_secs = static_cast<double>(elapsed_ns) / 1e9;
      if (run_secs <= 0.0) run_secs = 1e-9;
      s.avg_input_fps = static_cast<double>(ps.input_frames) / run_secs;
      s.avg_infer_fps = static_cast<double>(ps.infer_frames) / run_secs;
      s.avg_output_fps = static_cast<double>(ps.output_frames) / run_secs;
    }
    if (ps.output_frames > 0) {
      s.avg_detections_per_frame =
          static_cast<double>(ps.total_detections) / static_cast<double>(ps.output_frames);
    }
    out.push_back(std::move(s));
  }
  return out;
}

int MetricsRegistry::stream_count() const {
  std::lock_guard<std::mutex> lock(mu_);
  return static_cast<int>(streams_.size());
}

bool MetricsRegistry::valid_index(int idx) const {
  std::lock_guard<std::mutex> lock(mu_);
  return idx >= 0 && idx < static_cast<int>(streams_.size());
}

const std::string& MetricsRegistry::stream_id(int idx) const {
  static const std::string kEmpty;
  std::lock_guard<std::mutex> lock(mu_);
  if (idx < 0 || idx >= static_cast<int>(streams_.size())) {
    return kEmpty;
  }
  return streams_[static_cast<size_t>(idx)].stream_id;
}

}  // namespace metrics
}  // namespace jetedge
