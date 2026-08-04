// MetricsRegistry — minimal per-stream metrics (Stage 5) + latency tracking
// (Stage 12).
//
// Tracks per-stream frame counters and a fixed-window FPS estimate:
//   input_frames   — frames entering nvinfer (post-mux, per stream)
//   output_frames  — frames leaving nvtracker (per stream)
//   total_detections / frames_with_objects — per-frame detection counts
//
// Stage 12: per-frame inference-stage latency (input probe → output probe,
// i.e. nvinfer sink pad → nvtracker src pad; decode and RTSP jitter excluded).
// Samples live in a bounded ring per stream; percentiles are computed by the
// reporting thread from a copy, never by the probe threads.
//
// Thread-safe: pad probes run on the streaming thread, this is guarded by a
// single mutex.  Not a performance hotspot at these rates.

#pragma once

#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace jetedge {
namespace metrics {

class MetricsRegistry {
 public:
  // Clock injection for deterministic unit tests (monotonic ns by default).
  using ClockFn = std::function<uint64_t()>;

  explicit MetricsRegistry(ClockFn clock = MetricsRegistry::now_ns);

  // Register a stream and return its stable index (== mux pad_index).
  int register_stream(const std::string& stream_id);

  // Called from the input probe (nvinfer sink pad): frames entering inference.
  void on_input_frame(int stream_idx);

  // Called from the inference probe (nvinfer src pad): frames leaving inference.
  void on_infer_frame(int stream_idx);

  // Called from the output probe (nvtracker src pad): tracked frames.
  void on_output_frame(int stream_idx, int obj_count);

  // Stage 12 latency pairing.  frame_num must be the stable NvDsFrameMeta
  // frame_num; begin/end arrive in FIFO order per stream.  Both calls are
  // cheap (one lock + constant work) and safe on probe threads.
  void on_latency_begin(int stream_idx, uint64_t frame_num);
  void on_latency_end(int stream_idx, uint64_t frame_num);

  struct LatencyStats {
    uint64_t samples = 0;
    double avg_ms = 0.0;
    double p50_ms = 0.0;
    double p95_ms = 0.0;
    double p99_ms = 0.0;
    double max_ms = 0.0;
  };

  // All samples in the per-stream ring (most recent ~kLatencyRingCap frames).
  LatencyStats latency_stats(int stream_idx) const;

  // Samples with global seq > since_seq (benchmark window marker).  Monotonic
  // watermark: take latency_watermark() before the window, then slice.
  LatencyStats latency_stats_since(int stream_idx, uint64_t since_seq) const;

  // Raw latency samples (microseconds) with seq > since_seq, for honest
  // cross-stream pooling by the reporting layer.
  std::vector<uint32_t> latency_samples_since(int stream_idx,
                                              uint64_t since_seq) const;

  // Global monotonically increasing sample counter (shared across streams).
  uint64_t latency_watermark() const;

  // Nearest-rank percentiles over raw microseconds (shared by per-stream and
  // pooled computation).  Empty input → all-zero LatencyStats.
  static LatencyStats compute_stats_from_us(const std::vector<uint32_t>& us);

  // Diagnostics: pairing misses and evictions, for stall detection.
  uint64_t latency_desync_count(int stream_idx) const;
  uint64_t latency_evicted_count(int stream_idx) const;

  struct StreamSummary {
    std::string stream_id;
    uint64_t input_frames = 0;         // entering nvinfer
    uint64_t infer_frames = 0;         // leaving nvinfer
    uint64_t output_frames = 0;        // leaving nvtracker
    uint64_t frames_with_objects = 0;
    uint64_t total_detections = 0;
    double avg_input_fps = 0.0;        // over the whole run
    double avg_infer_fps = 0.0;        // over the whole run
    double avg_output_fps = 0.0;       // over the whole run
    double latest_input_fps = 0.0;     // 2s sliding window
    double latest_infer_fps = 0.0;     // 2s sliding window
    double latest_output_fps = 0.0;    // 2s sliding window
    double avg_detections_per_frame = 0.0;
    LatencyStats latency;              // Stage 12, ring samples
  };

  // All registered streams, ordered by stream index.
  std::vector<StreamSummary> snapshot() const;

  int stream_count() const;
  bool valid_index(int idx) const;
  const std::string& stream_id(int idx) const;

 private:
  static constexpr size_t kLatencyRingCap = 4096;   // ~136 s @ 30 fps per stream
  static constexpr size_t kPendingCap = 256;        // in-flight frames per stream
  static constexpr uint64_t kPendingMaxAgeNs = 5ULL * 1000 * 1000 * 1000;

  struct LatencySample {
    uint32_t seq = 0;         // global watermark at append
    uint32_t latency_us = 0;
  };

  struct PerStream {
    std::string stream_id;
    uint64_t input_frames = 0;
    uint64_t infer_frames = 0;
    uint64_t output_frames = 0;
    uint64_t frames_with_objects = 0;
    uint64_t total_detections = 0;
    uint64_t run_start_ns = 0;        // first frame arrival (for whole-run avg)
    uint64_t window_input = 0;        // counts within the current window
    uint64_t window_infer = 0;
    uint64_t window_output = 0;
    uint64_t window_start_ns = 0;
    double latest_input_fps = 0.0;
    double latest_infer_fps = 0.0;
    double latest_output_fps = 0.0;
    // Stage 12: latency ring + in-flight pairing queue (FIFO, fixed arrays).
    LatencySample ring[kLatencyRingCap];
    size_t ring_head = 0;             // next write slot
    size_t ring_count = 0;            // valid samples
    struct Pending { uint64_t frame_num; uint64_t t_start_ns; };
    Pending pending[kPendingCap];
    size_t pending_head = 0;          // dequeue position
    size_t pending_size = 0;
    uint64_t latency_desync = 0;
    uint64_t latency_evicted = 0;
  };

  std::vector<PerStream> streams_;
  mutable std::mutex mu_;
  ClockFn clock_;
  uint64_t latency_watermark_ = 0;

  static uint64_t now_ns();
  void maybe_roll_window(PerStream* ps, uint64_t now);
  LatencyStats compute_latency_stats(const PerStream& ps, uint64_t since_seq) const;
};

}  // namespace metrics
}  // namespace jetedge
