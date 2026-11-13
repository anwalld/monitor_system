#pragma once

// 急救采样：独立的 CPU 来源和忙碌度计算器（不与采集线程共享状态），急救期间用 IntervalGuard 把
// cpustat 刷新周期改成 1 ms；每次 sample() 重新读 CPU，其余字段取采集线程的最新样本。
// 只在一条急救流里使用（TaskService 保证同一时刻只有一条），本类不加锁。

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "collector/cpu_source.h"
#include "collector/interval_guard.h"
#include "monitor.pb.h"

namespace lm::worker {

class RescueSampler {
 public:
  using SourceFactory = std::function<std::unique_ptr<CpuSource>()>;
  using LatestFn = std::function<lm::Sample()>;

  RescueSampler(SourceFactory make_source, LatestFn latest, uint32_t rescue_interval_us = 1000)
      : make_source_(std::move(make_source)), latest_(std::move(latest)),
        interval_us_(rescue_interval_us) {}

  // 急救开始：创建 CPU 来源、改刷新周期、建基线。来源创建失败时 sample() 只返回最新样本。
  void begin();
  // 急救结束：恢复刷新周期，释放来源。
  void end();
  bool active() const { return started_; }
  bool interval_changed() const { return guard_ && guard_->active(); }

  lm::Sample sample();

 private:
  SourceFactory make_source_;
  LatestFn latest_;
  const uint32_t interval_us_;
  bool started_ = false;
  std::unique_ptr<CpuSource> source_;
  std::unique_ptr<IntervalGuard> guard_;
  CpuBusyCalculator calc_;
  CpuSnapshot snap_;
  std::vector<float> core_busy_;
  float cpu_busy_ = 0;
  bool has_busy_ = false;
};

}  // namespace lm::worker
