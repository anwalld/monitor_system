#include "probe_state.h"

#include <algorithm>
#include <cmath>

namespace lm::worker {

void ProbeState::on_sent(uint16_t seq, uint32_t target, int64_t sent_ns) {
  pending_.push_back({seq, target, sent_ns});
}

bool ProbeState::on_reply(uint16_t seq, uint32_t src, int64_t recv_ns) {
  for (auto it = pending_.begin(); it != pending_.end(); ++it) {
    if (it->seq != seq) continue;
    if (it->target != src) return false;  // 序列号对得上但来源不对：不认
    const double ms = static_cast<double>(std::max<int64_t>(0, recv_ns - it->sent_ns)) / 1e6;
    pending_.erase(it);
    ++ok_;
    sum_ms_ += ms;
    sumsq_ms_ += ms * ms;
    return true;
  }
  return false;  // 已超时出队的迟到回包，或根本不是我们发的
}

int ProbeState::expire(int64_t now_ns) {
  int n = 0;
  while (!pending_.empty() && now_ns - pending_.front().sent_ns > timeout_ns_) {
    pending_.pop_front();
    ++timeouts_;
    ++n;
  }
  return n;
}

ProbePeriod ProbeState::take() {
  ProbePeriod p;
  p.ok = ok_;
  p.timeouts = timeouts_;
  if (ok_ > 0) {
    const double n = static_cast<double>(ok_);
    p.rtt_ms = sum_ms_ / n;
    p.jitter_ms = std::sqrt(std::max(0.0, sumsq_ms_ / n - p.rtt_ms * p.rtt_ms));
  }
  if (ok_ + timeouts_ > 0) {
    p.timeout_rate = std::clamp(
        static_cast<double>(timeouts_) / static_cast<double>(ok_ + timeouts_), 0.0, 1.0);
  }
  ok_ = 0;
  timeouts_ = 0;
  sum_ms_ = 0.0;
  sumsq_ms_ = 0.0;
  return p;
}

}  // namespace lm::worker
