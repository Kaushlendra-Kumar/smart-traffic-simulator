#pragma once
// Bounded multi-producer / single-consumer queue for control commands.
//
// Guarantees:
//  * Thread-safe: any number of producer threads may push; one consumer drains.
//  * FIFO: commands are applied in the order their push() calls linearise.
//  * Bounded: push() never blocks; when full it returns false and the command is NOT
//    enqueued, so callers can report "busy" to the client. Commands are never dropped
//    silently (telemetry, by contrast, is coalesced — see the WebSocket gateway).

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <deque>
#include <mutex>
#include <vector>

namespace traffic {

template <typename T>
class CommandQueue {
 public:
  explicit CommandQueue(std::size_t capacity) : capacity_(capacity) {}

  bool push(T item) {
    {
      std::lock_guard lock(mu_);
      if (items_.size() >= capacity_) return false;
      items_.push_back(std::move(item));
    }
    cv_.notify_one();
    return true;
  }

  /// Moves all queued items into `out` (appending). Never blocks.
  void drain(std::vector<T>& out) {
    std::lock_guard lock(mu_);
    while (!items_.empty()) {
      out.push_back(std::move(items_.front()));
      items_.pop_front();
    }
  }

  /// Waits until an item is available or the deadline passes.
  template <typename Clock, typename Duration>
  void wait_until(const std::chrono::time_point<Clock, Duration>& deadline) {
    std::unique_lock lock(mu_);
    cv_.wait_until(lock, deadline, [&] { return !items_.empty() || woken_; });
    woken_ = false;
  }

  void wake() {
    {
      std::lock_guard lock(mu_);
      woken_ = true;
    }
    cv_.notify_all();
  }

  [[nodiscard]] std::size_t size() const {
    std::lock_guard lock(mu_);
    return items_.size();
  }
  [[nodiscard]] std::size_t capacity() const { return capacity_; }

 private:
  const std::size_t capacity_;
  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::deque<T> items_;
  bool woken_ = false;
};

}  // namespace traffic
