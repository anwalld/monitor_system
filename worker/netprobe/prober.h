#pragma once

// ICMP 探测线程（仅 Linux）：每块网卡一个绑定到该网卡的原始 ICMP socket，
// 和一个 100ms 的 timerfd、一个退出用的 eventfd 注册进同一个 epoll。
// 采集线程每周期调用 take_period() 取出并清零每块网卡的结果。

#include <cstdint>
#include <memory>
#include <vector>

#include "nic_config.h"
#include "probe_state.h"

namespace lm::worker {

class Prober {
 public:
  // 创建 socket 等资源；失败抛 std::runtime_error（附网卡名与原因）。非 Linux 直接抛出。
  Prober(const NetProbeConfig& cfg, const std::vector<NicSpec>& nics);
  ~Prober();
  Prober(const Prober&) = delete;
  Prober& operator=(const Prober&) = delete;

  void start();
  void stop();

  // 按网卡顺序返回本周期结果并清零。
  std::vector<ProbePeriod> take_period();

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace lm::worker
