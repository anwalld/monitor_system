#pragma once

// 一次急救：单独的 channel 打开 WorkerTask.Rescue 双向流，发送线程每 interval_ms 发一条请求、
// 每 batch_ms 结算一批并判断是否结束，接收线程收回应。结束时把节点转为慢启动或剔除。

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

#include <grpcpp/grpcpp.h>

#include "breaker/prober.h"
#include "breaker/rescue_logic.h"
#include "ingest/node_table.h"
#include "task.grpc.pb.h"

namespace lm::manager {

class RescueSession {
 public:
  RescueSession(NodeTable& table, RescueStart start, RescueParams p, StateSink sink);
  ~RescueSession();

  void start();
  bool done() const { return done_.load(); }
  void cancel() { cancel_.store(true); }
  void join();

  RescueStats stats() const;
  const std::string& node_id() const { return start_.node_id; }

 private:
  void run();

  NodeTable& table_;
  const RescueStart start_;
  const RescueParams p_;
  StateSink sink_;
  mutable std::mutex mu_;
  RescueTracker tracker_;
  std::atomic<bool> done_{false};
  std::atomic<bool> cancel_{false};
  std::thread thread_;
};

// 管理所有急救会话：同一节点同时最多一个。只在调度主循环线程里调用。
class RescueManager {
 public:
  RescueManager(NodeTable& table, RescueParams p, StateSink sink)
      : table_(table), p_(p), sink_(std::move(sink)) {}
  ~RescueManager() { stop(); }

  void launch(const RescueStart& s);
  void reap();  // 回收已结束的会话
  void stop();  // 取消并等待全部会话
  std::size_t active() const { return sessions_.size(); }
  uint64_t finished() const { return finished_; }

 private:
  NodeTable& table_;
  const RescueParams p_;
  StateSink sink_;
  std::map<std::string, std::unique_ptr<RescueSession>> sessions_;
  uint64_t finished_ = 0;
};

}  // namespace lm::manager
