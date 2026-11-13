#include "score.h"

#include <algorithm>
#include <cmath>

namespace lm::manager {

namespace {

double clamp01(double v) { return std::min(1.0, std::max(0.0, v)); }

// 非有限输入记一次 invalid 并按 0 分处理。
double norm_counted(double x, const Range& r, int* invalid) {
  if (!std::isfinite(x)) {
    if (invalid != nullptr) ++*invalid;
    return 0;
  }
  return normalize(x, r);
}

}  // namespace

double normalize(double x, const Range& r) {
  if (!std::isfinite(x)) return 0;
  return clamp01((r.bad - x) / (r.bad - r.good));
}

double normalize_log(double x, const Range& r) {
  if (!std::isfinite(x)) return 0;
  if (x <= 0) return 1;
  const double lg = std::log(r.good);
  const double lb = std::log(r.bad);
  return clamp01((lb - std::log(x)) / (lb - lg));
}

double nic_score(const lm::NicStat& nic, const NetParams& p, int* invalid) {
  const double a = norm_counted(nic.rtt_ms(), p.rtt_ms, invalid);
  const double b = norm_counted(nic.timeout_rate(), p.timeout, invalid);
  const double c = norm_counted(nic.retrans_rate(), p.retrans, invalid);
  return std::min({a, b, c});
}

ScoreBreakdown compute_score(const lm::Sample& s, const ScoringParams& p) {
  ScoreBreakdown b;
  b.cpu = norm_counted(s.cpu_busy(), p.cpu.range, &b.invalid);
  b.mem = norm_counted(s.mem_avail_ratio(), p.mem.range, &b.invalid);

  if (s.has_runq_p99_us()) {
    const double x = s.runq_p99_us();
    if (!std::isfinite(x)) ++b.invalid;
    b.runq = normalize_log(x, p.runq.range);
  } else {
    b.runq_missing = true;
    b.runq = p.missing_score;
  }

  if (s.nics_size() == 0) {
    b.net_missing = true;
    b.net = p.missing_score;
  } else {
    double best = -1;
    for (const auto& nic : s.nics()) {
      const double v = nic_score(nic, p.net, &b.invalid);
      if (v > best) {
        best = v;
        b.best_nic = nic.name();
      }
    }
    b.net = best;
  }

  const double wsum = p.cpu.weight + p.runq.weight + p.mem.weight + p.net.weight;
  b.total = (p.cpu.weight * b.cpu + p.runq.weight * b.runq + p.mem.weight * b.mem +
             p.net.weight * b.net) /
            wsum;
  return b;
}

}  // namespace lm::manager
