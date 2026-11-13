#pragma once

// 按权重随机选择：w = score × f_inflight × f_timeout × f_slow × f_pred。
// 抽样用前缀和 + 均匀随机数 + 二分查找。

#include <cstddef>
#include <optional>
#include <vector>

namespace lm::manager {

// 选择权重的修正因子。m5a 全部为 1；m5b 填 inflight、timeout，M6 填 slow，M9 填 pred。
struct Factors {
  double inflight = 1;
  double timeout = 1;
  double slow = 1;
  double pred = 1;
};

double weight(double score, const Factors& f);

// prefix 为权重前缀和（非递减），u ∈ [0, 1)。返回第一个前缀和 > u × 总权重的下标；
// 总权重 ≤ 0 或 prefix 为空时返回空。
std::optional<std::size_t> pick_weighted(const std::vector<double>& prefix, double u);

}  // namespace lm::manager
