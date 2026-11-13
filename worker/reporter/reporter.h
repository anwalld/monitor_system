#pragma once

// 上报线程：每个周期从环形缓冲取最新样本，带截止时间调用 Collector.Report；失败不重传。

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>

#include <grpcpp/grpcpp.h>

#include "lm/log.h"
#include "lm/ring.h"
#include "monitor.grpc.pb.h"

namespace lm::worker {

class Reporter {
 public:
  using SampleRing = lm::Ring<lm::Sample, 8>;
  // 样本旁路输出（--dry-run 打印用）；设置后不再调用 RPC。
  using Sink = std::function<void(const lm::Sample&)>;

  struct Options {
    std::chrono::milliseconds interval{1000};
    std::chrono::milliseconds deadline{500};
    // 首次上报相对 start() 的延迟。采集与上报周期相同且同时启动时，两者节拍几乎重合，
    // 抖动会让上报偶尔早于本周期样本、下一拍又跳过一条；错开半个周期可保证每拍恰好有一条新样本。
    // 小于 0 表示取 interval / 2。
    std::chrono::milliseconds phase{-1};
  };

  Reporter(SampleRing& ring, std::shared_ptr<grpc::Channel> channel, Options opt);
  Reporter(SampleRing& ring, Sink sink, Options opt);
  ~Reporter();

  void start();
  void stop();

  // 执行一次：取最新样本并发送。返回是否发出了一次调用（无新样本时为 false）。
  bool report_once();

  uint64_t sent_ok() const { return ok_.load(); }
  uint64_t sent_failed() const { return failed_.load(); }
  uint64_t last_sent_seq() const { return last_seq_.load(); }

 private:
  void run();

  SampleRing& ring_;
  std::unique_ptr<lm::Collector::Stub> stub_;
  Sink sink_;
  const Options opt_;

  lm::RateLimiter fail_log_{std::chrono::seconds(10)};
  std::atomic<uint64_t> last_seq_{0};
  std::atomic<uint64_t> ok_{0};
  std::atomic<uint64_t> failed_{0};
  std::atomic<bool> running_{false};
  std::thread thread_;
};

}  // namespace lm::worker
