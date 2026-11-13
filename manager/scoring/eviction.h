#pragma once

// 关键指标剔除：越过剔除线连续 evict_after 次才剔除，回到恢复线以内连续 recover_after 次才恢复
// （迟滞 + 去抖）；越过剔除线 immediate_factor 倍立即剔除。纯状态机，调用方负责加锁。

#include <string>

#include "monitor.pb.h"
#include "scoring_params.h"

namespace lm::manager {

struct GateParams {
  double evict = 1;
  double recover = 0;
  bool higher_is_worse = true;
  int evict_after = 3;
  int recover_after = 5;
  double immediate_factor = 10;
};

class MetricGate {
 public:
  // 用一个观测值更新；x 非有限视为缺失，计数不变。
  void update(double x, const GateParams& p);
  bool evicted() const { return evicted_; }
  int bad_streak() const { return bad_streak_; }
  int good_streak() const { return good_streak_; }

 private:
  bool evicted_ = false;
  int bad_streak_ = 0;
  int good_streak_ = 0;
};

class EvictionTracker {
 public:
  // 用一条样本更新四个关键指标；样本缺少的指标保持原状。
  void update(const lm::Sample& s, const ScoringParams& p);
  bool evicted() const;
  // 处于剔除状态的指标名，按 cpu、runq、mem、net 的顺序用逗号连接；未剔除时为空串。
  std::string reason() const;

  const MetricGate& cpu() const { return cpu_; }
  const MetricGate& runq() const { return runq_; }
  const MetricGate& mem() const { return mem_; }
  const MetricGate& net() const { return net_; }

 private:
  MetricGate cpu_, runq_, mem_, net_;
};

}  // namespace lm::manager
