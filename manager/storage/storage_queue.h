#pragma once

// 调度路径 → 存储线程的有界队列：多生产者（gRPC 线程）、单消费者（存储线程）。
// 满了丢最旧的一条并计数，保证 Redis 长时间不可用时内存不增长。

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

namespace lm::manager {

template <typename T>
class BoundedQueue {
 public:
  explicit BoundedQueue(std::size_t capacity) : capacity_(capacity) {}

  void push(T v) {
    {
      std::lock_guard<std::mutex> lk(mu_);
      if (q_.size() >= capacity_) {
        q_.pop_front();
        ++dropped_;
      }
      q_.push_back(std::move(v));
    }
    cv_.notify_one();
  }

  // 等待直到攒够 max_items 条或超时，取出至多 max_items 条（可能为空）。
  std::vector<T> pop_batch(std::size_t max_items, std::chrono::milliseconds wait) {
    std::unique_lock<std::mutex> lk(mu_);
    cv_.wait_for(lk, wait, [&] { return q_.size() >= max_items || closed_; });
    std::vector<T> out;
    const std::size_t n = std::min(max_items, q_.size());
    out.reserve(n);
    for (std::size_t i = 0; i < n; ++i) {
      out.push_back(std::move(q_.front()));
      q_.pop_front();
    }
    return out;
  }

  void close() {
    {
      std::lock_guard<std::mutex> lk(mu_);
      closed_ = true;
    }
    cv_.notify_all();
  }

  std::size_t size() const {
    std::lock_guard<std::mutex> lk(mu_);
    return q_.size();
  }

  uint64_t dropped() const {
    std::lock_guard<std::mutex> lk(mu_);
    return dropped_;
  }

 private:
  const std::size_t capacity_;
  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::deque<T> q_;
  uint64_t dropped_ = 0;
  bool closed_ = false;
};

}  // namespace lm::manager
