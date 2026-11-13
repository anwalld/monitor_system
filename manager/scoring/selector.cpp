#include "selector.h"

#include <algorithm>
#include <cmath>

namespace lm::manager {

double weight(double score, const Factors& f) {
  const double w = score * f.inflight * f.timeout * f.slow * f.pred;
  return std::isfinite(w) && w > 0 ? w : 0;
}

std::optional<std::size_t> pick_weighted(const std::vector<double>& prefix, double u) {
  if (prefix.empty()) return std::nullopt;
  const double total = prefix.back();
  if (!(total > 0)) return std::nullopt;
  u = std::min(std::max(u, 0.0), std::nextafter(1.0, 0.0));
  const double target = u * total;
  auto it = std::upper_bound(prefix.begin(), prefix.end(), target);
  std::size_t i = static_cast<std::size_t>(it - prefix.begin());
  if (it == prefix.end()) {
    // 浮点误差兜底：退到最后一个权重大于 0 的节点
    i = prefix.size() - 1;
    while (i > 0 && prefix[i] <= prefix[i - 1]) --i;
  }
  return i;
}

}  // namespace lm::manager
