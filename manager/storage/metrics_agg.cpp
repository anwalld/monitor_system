#include "metrics_agg.h"

#include <algorithm>

namespace lm::manager {

void MetricsAggregator::add(const MetricPoint& p) {
  Acc& a = acc_[{p.node_id, bucket_of(p.ts_ms)}];
  if (a.n == 0) {
    a.cpu_max = p.cpu;
    a.mem_min = p.mem;
  }
  a.cpu_sum += p.cpu;
  a.cpu_max = std::max(a.cpu_max, p.cpu);
  a.mem_min = std::min(a.mem_min, p.mem);
  if (p.runq_p99_us) a.runq_max = a.runq_max ? std::max(*a.runq_max, *p.runq_p99_us) : *p.runq_p99_us;
  if (p.rtt_ms) {
    a.rtt_sum += *p.rtt_ms;
    a.to_sum += p.timeout_rate.value_or(0);
    a.rt_sum += p.retrans_rate.value_or(0);
    ++a.net_n;
  }
  a.score_sum += p.score;
  ++a.n;
}

MetricsRow MetricsAggregator::finish(const std::pair<std::string, int64_t>& key, const Acc& a) {
  MetricsRow r;
  r.node_id = key.first;
  r.bucket_start_ms = key.second;
  r.samples = a.n;
  r.cpu_avg = a.cpu_sum / a.n;
  r.cpu_max = a.cpu_max;
  r.runq_p99_max_us = a.runq_max;
  r.mem_avail_min = a.mem_min;
  if (a.net_n > 0) {
    r.net_rtt_avg_ms = a.rtt_sum / a.net_n;
    r.net_timeout_rate = a.to_sum / a.net_n;
    r.net_retrans_rate = a.rt_sum / a.net_n;
  }
  r.score_avg = a.score_sum / a.n;
  return r;
}

std::vector<MetricsRow> MetricsAggregator::take_finished(int64_t now_ms) {
  std::vector<MetricsRow> out;
  for (auto it = acc_.begin(); it != acc_.end();) {
    if (it->first.second + bucket_ms_ + grace_ms_ <= now_ms) {
      out.push_back(finish(it->first, it->second));
      it = acc_.erase(it);
    } else {
      ++it;
    }
  }
  return out;
}

std::vector<MetricsRow> MetricsAggregator::take_all() {
  std::vector<MetricsRow> out;
  for (const auto& [k, a] : acc_) out.push_back(finish(k, a));
  acc_.clear();
  return out;
}

}  // namespace lm::manager
