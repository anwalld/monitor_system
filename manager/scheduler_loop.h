#pragma once

// 调度主循环：每 100 ms 处理在途账的超时链表头，每秒更新在途基线、衰减超时计数 T。
// 锁内只收集过期任务，日志在锁外打（限频）。M6 的状态机和低频探测也挂在这里。

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#include "ingest/node_table.h"

namespace lm::manager {

class SchedulerLoop {
 public:
  struct Options {
    std::chrono::milliseconds tick{100};
    std::chrono::milliseconds second{1000};
  };

  SchedulerLoop(NodeTable& table, Options opt) : table_(table), opt_(opt) {}
  ~SchedulerLoop() { stop(); }

  void start();
  void stop();

  // 执行一拍（测试可直接调用）。
  void tick_once(int64_t now_mono_ms);

  uint64_t voided() const { return voided_.load(); }
  uint64_t unconfirmed() const { return unconfirmed_.load(); }
  uint64_t exec_timeouts() const { return exec_timeouts_.load(); }

 private:
  void run();

  NodeTable& table_;
  const Options opt_;
  int64_t last_second_ms_ = -1;
  std::vector<InflightLedger::Expired> expired_;
  int detail_budget_ = 10;  // 本秒内还能逐条记录的超时任务数
  std::atomic<uint64_t> voided_{0};
  std::atomic<uint64_t> unconfirmed_{0};
  std::atomic<uint64_t> exec_timeouts_{0};
  std::atomic<bool> running_{false};
  std::thread thread_;
};

}  // namespace lm::manager
