#pragma once

// 低频探测线程：每 probe_interval_ms 对所有新鲜、不在急救中的节点并发调用一次 WorkerTask.Probe
// （gRPC 异步 + 一个 CompletionQueue，截止 probe_timeout_ms），结果逐个写回节点表。

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>

#include <grpcpp/grpcpp.h>

#include "ingest/node_table.h"
#include "task.grpc.pb.h"

namespace lm::manager {

using StateSink = std::function<void(const StateChange&)>;

class Prober {
 public:
  Prober(NodeTable& table, const BreakerParams& p, StateSink sink)
      : table_(table), interval_ms_(p.probe_interval_ms), timeout_ms_(p.probe_timeout_ms),
        sink_(std::move(sink)) {}
  ~Prober() { stop(); }

  void start();
  void stop();
  void probe_round();  // 一轮探测（测试可直接调用）

  uint64_t ok() const { return ok_.load(); }
  uint64_t failed() const { return failed_.load(); }

 private:
  lm::WorkerTask::Stub* stub_for(const std::string& addr);
  void run();

  NodeTable& table_;
  const int64_t interval_ms_;
  const int64_t timeout_ms_;
  StateSink sink_;
  std::map<std::string, std::unique_ptr<lm::WorkerTask::Stub>> stubs_;  // 只在探测线程里用
  uint64_t seq_ = 0;
  std::atomic<uint64_t> ok_{0};
  std::atomic<uint64_t> failed_{0};
  std::atomic<bool> running_{false};
  std::thread thread_;
};

}  // namespace lm::manager
