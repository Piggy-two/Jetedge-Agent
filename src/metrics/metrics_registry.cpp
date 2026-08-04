// MetricsRegistry implementation (Stage 5) + latency tracking (Stage 12).

#include "jetedge/metrics/metrics_registry.h"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <vector>

#include "jetedge/common/logging.h"

namespace jetedge {
namespace metrics {

namespace {
constexpr uint64_t kWindowDurationNs = 2ULL * 1000 * 1000 * 1000;  // 2s window
}

MetricsRegistry::MetricsRegistry(ClockFn clock) : clock_(std::move(clock)) {
  if (!clock_) clock_ = MetricsRegistry::now_ns;
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
  const uint64_t t = clock_();
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
  maybe_roll_window(ps, clock_());
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
  maybe_roll_window(ps, clock_());
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
  maybe_roll_window(ps, clock_());
}

void MetricsRegistry::on_latency_begin(int stream_idx, uint64_t frame_num) {
  if (stream_idx < 0 || stream_idx >= static_cast<int>(streams_.size())) {
    LOG_WARN("metrics", "on_latency_begin: invalid stream index %d", stream_idx);
    return;
  }
  std::lock_guard<std::mutex> lock(mu_);
  PerStream* ps = &streams_[static_cast<size_t>(stream_idx)];
  const uint64_t now = clock_();
  // Age out stale entries (stream stalled / frames never completed).
  while (ps->pending_size > 0) {
    const size_t front = (ps->pending_head) % kPendingCap;
    if (now < ps->pending[front].t_start_ns) break;  // clock anomaly
    const uint64_t age = now - ps->pending[front].t_start_ns;
    if (age <= kPendingMaxAgeNs) break;
    ps->pending_head = (ps->pending_head + 1) % kPendingCap;
    --ps->pending_size;
    ++ps->latency_evicted;
  }
  // Overflow: evict the oldest in-flight frame.
  if (ps->pending_size == kPendingCap) {
    ps->pending_head = (ps->pending_head + 1) % kPendingCap;
    --ps->pending_size;
    ++ps->latency_evicted;
  }
  const size_t slot = (ps->pending_head + ps->pending_size) % kPendingCap;
  ps->pending[slot] = {frame_num, now};
  ++ps->pending_size;
}

void MetricsRegistry::on_latency_end(int stream_idx, uint64_t frame_num) {
  if (stream_idx < 0 || stream_idx >= static_cast<int>(streams_.size())) {
    LOG_WARN("metrics", "on_latency_end: invalid stream index %d", stream_idx);
    return;
  }
  std::lock_guard<std::mutex> lock(mu_);
  PerStream* ps = &streams_[static_cast<size_t>(stream_idx)];
  const uint64_t now = clock_();
  // FIFO scan (nvinfer/nvtracker preserve order; front hit expected).
  size_t found = ps->pending_size;
  for (size_t i = 0; i < ps->pending_size; ++i) {
    const size_t idx = (ps->pending_head + i) % kPendingCap;
    if (ps->pending[idx].frame_num == frame_num) {
      found = i;
      break;
    }
  }
  if (found == ps->pending_size) {
    ++ps->latency_desync;
    return;
  }
  const size_t pos = (ps->pending_head + found) % kPendingCap;
  const uint64_t t_start = ps->pending[pos].t_start_ns;
  // Remove the entry: shift everything after it down one slot.
  for (size_t i = found; i > 0; --i) {
    const size_t cur = (ps->pending_head + i) % kPendingCap;
    const size_t prev = (ps->pending_head + i - 1) % kPendingCap;
    ps->pending[cur] = ps->pending[prev];
  }
  ps->pending_head = (ps->pending_head + 1) % kPendingCap;
  --ps->pending_size;
  if (now < t_start) {  // clock anomaly: drop the sample
    ++ps->latency_desync;
    return;
  }
  const uint32_t latency_us =
      static_cast<uint32_t>((now - t_start) / 1000ULL);
  LatencySample& s = ps->ring[ps->ring_head];
  s.seq = static_cast<uint32_t>(++latency_watermark_);
  s.latency_us = latency_us;
  ps->ring_head = (ps->ring_head + 1) % kLatencyRingCap;
  if (ps->ring_count < kLatencyRingCap) ++ps->ring_count;
}

MetricsRegistry::LatencyStats MetricsRegistry::compute_stats_from_us(
    const std::vector<uint32_t>& us) {
  LatencyStats out;
  out.samples = us.size();
  if (us.empty()) return out;
  std::vector<uint32_t> sorted = us;
  std::sort(sorted.begin(), sorted.end());
  uint64_t sum = 0;
  for (const uint32_t v : sorted) sum += v;
  out.avg_ms = static_cast<double>(sum) / static_cast<double>(sorted.size()) / 1000.0;
  out.max_ms = static_cast<double>(sorted.back()) / 1000.0;
  auto pct = [&sorted](double p) {
    const size_t idx = static_cast<size_t>(
        std::ceil(p * static_cast<double>(sorted.size()) / 100.0));
    return static_cast<double>(sorted[idx > 0 ? idx - 1 : 0]) / 1000.0;
  };
  out.p50_ms = pct(50.0);
  out.p95_ms = pct(95.0);
  out.p99_ms = pct(99.0);
  return out;
}

MetricsRegistry::LatencyStats MetricsRegistry::compute_latency_stats(
    const PerStream& ps, uint64_t since_seq) const {
  std::vector<uint32_t> us;
  us.reserve(ps.ring_count);
  for (size_t i = 0; i < ps.ring_count; ++i) {
    const size_t idx = (ps.ring_head + kLatencyRingCap - ps.ring_count + i) %
                       kLatencyRingCap;
    if (ps.ring[idx].seq <= since_seq) continue;
    us.push_back(ps.ring[idx].latency_us);
  }
  return compute_stats_from_us(us);
}

std::vector<uint32_t> MetricsRegistry::latency_samples_since(
    int stream_idx, uint64_t since_seq) const {
  if (stream_idx < 0 || stream_idx >= static_cast<int>(streams_.size())) {
    LOG_WARN("metrics", "latency_samples_since: invalid stream index %d",
             stream_idx);
    return {};
  }
  std::lock_guard<std::mutex> lock(mu_);
  const PerStream& ps = streams_[static_cast<size_t>(stream_idx)];
  std::vector<uint32_t> us;
  us.reserve(ps.ring_count);
  for (size_t i = 0; i < ps.ring_count; ++i) {
    const size_t idx = (ps.ring_head + kLatencyRingCap - ps.ring_count + i) %
                       kLatencyRingCap;
    if (ps.ring[idx].seq <= since_seq) continue;
    us.push_back(ps.ring[idx].latency_us);
  }
  return us;
}

MetricsRegistry::LatencyStats MetricsRegistry::latency_stats(int stream_idx) const {
  if (stream_idx < 0 || stream_idx >= static_cast<int>(streams_.size())) {
    LOG_WARN("metrics", "latency_stats: invalid stream index %d", stream_idx);
    return {};
  }
  std::lock_guard<std::mutex> lock(mu_);
  return compute_latency_stats(streams_[static_cast<size_t>(stream_idx)], 0);
}

MetricsRegistry::LatencyStats MetricsRegistry::latency_stats_since(
    int stream_idx, uint64_t since_seq) const {
  if (stream_idx < 0 || stream_idx >= static_cast<int>(streams_.size())) {
    LOG_WARN("metrics", "latency_stats_since: invalid stream index %d",
             stream_idx);
    return {};
  }
  std::lock_guard<std::mutex> lock(mu_);
  return compute_latency_stats(streams_[static_cast<size_t>(stream_idx)], since_seq);
}

uint64_t MetricsRegistry::latency_watermark() const {
  std::lock_guard<std::mutex> lock(mu_);
  return latency_watermark_;
}

uint64_t MetricsRegistry::latency_desync_count(int stream_idx) const {
  if (stream_idx < 0 || stream_idx >= static_cast<int>(streams_.size())) return 0;
  std::lock_guard<std::mutex> lock(mu_);
  return streams_[static_cast<size_t>(stream_idx)].latency_desync;
}

uint64_t MetricsRegistry::latency_evicted_count(int stream_idx) const {
  if (stream_idx < 0 || stream_idx >= static_cast<int>(streams_.size())) return 0;
  std::lock_guard<std::mutex> lock(mu_);
  return streams_[static_cast<size_t>(stream_idx)].latency_evicted;
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
      const uint64_t elapsed_ns = clock_() - ps.run_start_ns;
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
    s.latency = compute_latency_stats(ps, 0);
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
