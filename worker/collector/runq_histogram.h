#pragma once

// 调度延迟直方图的纯计算：周期差分、分位数（桶内线性插值）、丢弃率。与平台无关，macOS 上也能单测。
// 分桶公式见 bpf/runqlat.h，内核程序与这里共用。

#include <array>
#include <cstdint>
#include <optional>

#include "runqlat.h"

namespace lm::worker {

// 内核 map 的累计值（各核已求和）。
struct RunqCumulative {
  std::array<uint64_t, RUNQ_BUCKETS> buckets{};
  uint64_t drops = 0;
};

// 一个周期的统计结果。
struct RunqStats {
  float p50_us = 0.0F;
  float p99_us = 0.0F;
  float drop_rate = 0.0F;
  uint64_t samples = 0;  // 本周期样本数 N
  uint64_t drops = 0;    // 本周期丢弃数
};

// 按秩 r = p × N 找到所在桶，在 [下界, 下界 + 宽度) 内按比例插值。N 为 0 时返回 0。
double runq_percentile(const std::array<uint64_t, RUNQ_BUCKETS>& delta, double p);

// Δdrops / (Δdrops + N)，分母为 0 时为 0，截断到 [0, 1]。
double runq_drop_rate(uint64_t drops, uint64_t samples);

// 由相邻两次累计值得到本周期统计。第一次只存基线返回 nullopt；
// 累计值倒退（程序被重新加载等）时以本次为新基线并返回 nullopt。
class RunqWindow {
 public:
  std::optional<RunqStats> update(const RunqCumulative& cur);
  // 本周期差分（update 返回值有效时可用），供对照工具输出完整直方图。
  const std::array<uint64_t, RUNQ_BUCKETS>& last_delta() const { return delta_; }
  void reset() { has_prev_ = false; }

 private:
  bool has_prev_ = false;
  RunqCumulative prev_;
  std::array<uint64_t, RUNQ_BUCKETS> delta_{};
};

}  // namespace lm::worker
