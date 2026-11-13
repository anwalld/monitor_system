#include "node_state.h"

#include <algorithm>

namespace lm::manager {

const char* mode_name(NodeMode m) {
  switch (m) {
    case NodeMode::kSlowStart:
      return "slow_start";
    case NodeMode::kNormal:
      return "normal";
    case NodeMode::kEvicted:
      return "evicted";
    case NodeMode::kRescue:
      return "rescue";
  }
  return "?";
}

double slow_factor(const BreakerState& b, int64_t now_ms, const BreakerParams& p) {
  if (b.mode != NodeMode::kSlowStart) return 1.0;
  const double frac =
      std::min(1.0, std::max(0.0, static_cast<double>(now_ms - b.since_ms) / p.slow_start_ms));
  return p.slow_start_from + (1 - p.slow_start_from) * frac;
}

}  // namespace lm::manager
