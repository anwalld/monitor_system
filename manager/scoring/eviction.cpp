#include "eviction.h"

#include <algorithm>
#include <cmath>

namespace lm::manager {

void MetricGate::update(double x, const GateParams& p) {
  if (!std::isfinite(x)) return;
  const bool hw = p.higher_is_worse;
  const bool over = hw ? x >= p.evict : x <= p.evict;
  if (!evicted_) {
    const bool immediate = hw ? x >= p.evict * p.immediate_factor : x <= p.evict / p.immediate_factor;
    bad_streak_ = over ? bad_streak_ + 1 : 0;
    if (immediate || bad_streak_ >= p.evict_after) {
      evicted_ = true;
      bad_streak_ = 0;
      good_streak_ = 0;
    }
    return;
  }
  const bool within = hw ? x <= p.recover : x >= p.recover;
  good_streak_ = within ? good_streak_ + 1 : 0;
  if (good_streak_ >= p.recover_after) {
    evicted_ = false;
    good_streak_ = 0;
    bad_streak_ = 0;
  }
}

namespace {

GateParams gate_of(const MetricParams& m, const ScoringParams& p) {
  return {m.evict, m.recover, m.higher_is_worse(), p.evict_after, p.recover_after,
          p.immediate_factor};
}

}  // namespace

void EvictionTracker::update(const lm::Sample& s, const ScoringParams& p) {
  cpu_.update(s.cpu_busy(), gate_of(p.cpu, p));
  mem_.update(s.mem_avail_ratio(), gate_of(p.mem, p));
  if (s.has_runq_p99_us()) runq_.update(s.runq_p99_us(), gate_of(p.runq, p));
  if (s.nics_size() > 0) {
    double min_timeout = 1e300;
    for (const auto& n : s.nics()) {
      if (std::isfinite(n.timeout_rate())) min_timeout = std::min(min_timeout, double{n.timeout_rate()});
    }
    if (min_timeout < 1e300) {
      net_.update(min_timeout, GateParams{p.net.evict, p.net.recover, true, p.evict_after,
                                          p.recover_after, p.immediate_factor});
    }
  }
}

bool EvictionTracker::evicted() const {
  return cpu_.evicted() || runq_.evicted() || mem_.evicted() || net_.evicted();
}

std::string EvictionTracker::reason() const {
  std::string r;
  auto add = [&r](bool on, const char* name) {
    if (!on) return;
    if (!r.empty()) r += ',';
    r += name;
  };
  add(cpu_.evicted(), "cpu");
  add(runq_.evicted(), "runq");
  add(mem_.evicted(), "mem");
  add(net_.evicted(), "net");
  return r;
}

}  // namespace lm::manager
