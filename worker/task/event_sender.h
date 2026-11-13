#pragma once

// 任务事件发送：有界队列 + 一个发送线程，按入队顺序调用 Collector.OnTaskEvent。
// 每次截止 500 ms，最多尝试 3 次（间隔 200 ms、400 ms），仍失败则丢弃并计数；队列满时丢最旧。
// DONE 丢了 Manager 要等 300 秒执行超时并记一次 T，所以事件值得重试（样本上报则不重传）。

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

#include <grpcpp/grpcpp.h>

#include "lm/log.h"
#include "monitor.grpc.pb.h"

namespace lm::worker {

class EventSender {
 public:
  using SendFn = std::function<grpc::Status(const lm::TaskEvent&, std::chrono::milliseconds)>;

  struct Options {
    std::size_t capacity = 1024;
    std::chrono::milliseconds deadline{500};
    int max_attempts = 3;
    std::chrono::milliseconds backoff{200};  // 第 k 次重试前等 backoff × 2^(k−1)
  };

  EventSender(std::shared_ptr<grpc::Channel> channel, Options opt);
  EventSender(SendFn send, Options opt);  // 测试用
  ~EventSender();

  void start();
  void stop();  // 停止前尽量发完队列（每条只试一次）

  void enqueue(lm::TaskEvent e);

  uint64_t sent() const { return sent_.load(); }
  uint64_t dropped_failed() const { return dropped_failed_.load(); }
  uint64_t dropped_full() const { return dropped_full_.load(); }
  std::size_t queued() const;

 private:
  void run();
  bool send_with_retry(const lm::TaskEvent& e, bool retry);

  std::unique_ptr<lm::Collector::Stub> stub_;
  SendFn send_;
  const Options opt_;
  mutable std::mutex mu_;
  std::condition_variable cv_;
  std::deque<lm::TaskEvent> q_;
  bool stopping_ = false;
  bool started_ = false;
  lm::RateLimiter fail_log_{std::chrono::seconds(10)};
  std::atomic<uint64_t> sent_{0};
  std::atomic<uint64_t> dropped_failed_{0};
  std::atomic<uint64_t> dropped_full_{0};
  std::thread thread_;
};

}  // namespace lm::worker
