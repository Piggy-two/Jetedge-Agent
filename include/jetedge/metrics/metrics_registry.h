// MetricsRegistry — minimal per-stream metrics (Stage 5).
//
// Tracks per-stream frame counters and a fixed-window FPS estimate:
//   input_frames   — frames entering nvinfer (post-mux, per stream)
//   output_frames  — frames leaving nvtracker (per stream)
//   total_detections / frames_with_objects — per-frame detection counts
//
// Thread-safe: pad probes run on the streaming thread, this is guarded by a
// single mutex.  Not a performance hotspot at these rates.

#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace jetedge {
namespace metrics {

class MetricsRegistry {
 public:
  // Register a stream and return its stable index (== mux pad_index).
  int register_stream(const std::string& stream_id);

  // Called from the input probe (nvinfer sink pad): frames entering inference.
  void on_input_frame(int stream_idx);

  // Called from the inference probe (nvinfer src pad): frames leaving inference.
  void on_infer_frame(int stream_idx);

  // Called from the output probe (nvtracker src pad): tracked frames.
  void on_output_frame(int stream_idx, int obj_count);

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
  };

  // All registered streams, ordered by stream index.
  std::vector<StreamSummary> snapshot() const;

  int stream_count() const;
  bool valid_index(int idx) const;
  const std::string& stream_id(int idx) const;

 private:
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
  };

  std::vector<PerStream> streams_;
  mutable std::mutex mu_;

  static uint64_t now_ns();
  void maybe_roll_window(PerStream* ps, uint64_t now);
};

}  // namespace metrics
}  // namespace jetedge
