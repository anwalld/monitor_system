#pragma once

// 评分与剔除参数。缺省值取开发文档“默认参数”表，可由 manager.yaml 的 scoring 段覆盖。

#include <cstdint>

#include "lm/config.h"

namespace lm::manager {

// 一个指标的归一化区间：s = clamp((bad − x) / (bad − good), 0, 1)。
struct Range {
  double good = 0;
  double bad = 1;
};

// 关键指标：归一化区间 + 剔除线 + 恢复线 + 权重。
struct MetricParams {
  Range range;
  double evict = 1;
  double recover = 0;
  double weight = 0;
  // 越大越坏（good < bad）返回 true。
  bool higher_is_worse() const { return range.good < range.bad; }
};

struct NetParams {
  Range rtt_ms{5, 100};
  Range timeout{0, 0.1};
  Range retrans{0, 0.05};
  double evict = 0.5;    // 所有网卡超时率都 ≥ evict
  double recover = 0.2;  // 最小超时率 ≤ recover
  double weight = 0.2;
};

struct ScoringParams {
  MetricParams cpu{{0.2, 0.9}, 0.97, 0.85, 0.3};
  MetricParams runq{{100, 2000}, 20000, 5000, 0.3};  // 微秒，取对数映射
  MetricParams mem{{0.5, 0.1}, 0.05, 0.1, 0.2};
  NetParams net;
  double missing_score = 0.3;
  int64_t lost_after_ms = 3000;
  int evict_after = 3;
  int recover_after = 5;
  double immediate_factor = 10;

  // 从配置的 scoring 段读取，缺失项用缺省值；校验失败抛 lm::ConfigError（信息含键名）。
  static ScoringParams from_config(const lm::Config& cfg);
  // 校验当前参数；失败抛 lm::ConfigError。
  void validate() const;
};

}  // namespace lm::manager
