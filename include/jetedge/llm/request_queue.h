// BoundedPriorityQueue — bounded, priority-ordered, thread-safe queue for
// async LLM requests (Stage 7).
//
// - `enqueue(item, priority)`: returns false when the queue is full; the
//   lowest-priority (then oldest) item is shed to make room.  Non-blocking.
// - `dequeue(timeout)`: blocks up to `timeout` for the highest-priority
//   item; returns nullopt on timeout or after shutdown().
// - `shutdown()`: wakes all waiters; dequeue() then returns nullopt.
//
// Priority: lower enum value first; ties broken by sequence number (FIFO).

#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <queue>
#include <utility>

#include "jetedge/llm/llm_types.h"

namespace jetedge {
namespace llm {

template <typename T>
class BoundedPriorityQueue {
 public:
  explicit BoundedPriorityQueue(size_t max_size) : max_size_(max_size) {}

  BoundedPriorityQueue(const BoundedPriorityQueue&) = delete;
  BoundedPriorityQueue& operator=(const BoundedPriorityQueue&) = delete;

  // Push an item.  Returns false if the queue is full after shedding the
  // lowest-priority (then oldest) item.  Never blocks.
  bool enqueue(T item, RequestPriority priority) {
    std::lock_guard<std::mutex> lock(mu_);
    if (shutdown_) {
      return false;
    }
    Entry e;
    e.item = std::move(item);
    e.priority = priority;
    e.seq = next_seq_++;
    if (queue_.size() >= max_size_) {
      // Shed the lowest-priority item (last in the heap order).
      queue_.pop();
    }
    queue_.push(std::move(e));
    cv_.notify_one();
    return true;
  }

  // Pop the highest-priority item, waiting up to `timeout`.
  // Returns nullopt on timeout or after shutdown().
  std::optional<T> dequeue(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lock(mu_);
    if (queue_.empty() && !shutdown_) {
      cv_.wait_for(lock, timeout);
    }
    if (queue_.empty() || shutdown_) {
      return std::nullopt;
    }
    Entry e = std::move(const_cast<Entry&>(queue_.top()));
    queue_.pop();
    return std::optional<T>(std::move(e.item));
  }

  // Wake all waiters; subsequent dequeue() calls return nullopt.
  void shutdown() {
    std::lock_guard<std::mutex> lock(mu_);
    shutdown_ = true;
    cv_.notify_all();
  }

  size_t size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return queue_.size();
  }

  bool empty() const {
    std::lock_guard<std::mutex> lock(mu_);
    return queue_.empty();
  }

 private:
  struct Entry {
    T item;
    RequestPriority priority = RequestPriority::kNormal;
    uint64_t seq = 0;

    // std::priority_queue is a max-heap by operator< — "greatest" item pops
    // first.  Invert: the "greatest" is the lowest priority value
    // (highest priority) with the smallest seq.
    bool operator<(const Entry& other) const {
      if (priority != other.priority) {
        return priority > other.priority;  // higher priority (lower enum) first
      }
      return seq > other.seq;  // older first
    }
  };

  size_t max_size_;
  uint64_t next_seq_ = 0;
  bool shutdown_ = false;
  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::priority_queue<Entry> queue_;
};

}  // namespace llm
}  // namespace jetedge
