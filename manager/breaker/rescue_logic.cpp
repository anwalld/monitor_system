#include "rescue_logic.h"

#include <algorithm>
#include <cmath>

namespace lm::manager {

void RescueTracker::on_send(uint64_t seq, int64_t now_us) {
  pending_.emplace_back(seq, now_us);
  ++stats_.sent;
}

void RescueTracker::timeout_one() {
  pending_.pop_front();
  ++stats_.timeouts;
  ++batch_timeouts_;
  if (!outage_) {
    outage_ = true;
    ++stats_.events;
  }
}

void RescueTracker::success_one() {
  pending_.pop_front();
  ++stats_.ok;
  outage_ = false;
}

void RescueTracker::on_response(uint64_t seq, int64_t /*now_us*/) {
  if (pending_.empty() || seq < pending_.front().first) {
    ++stats_.late;  // 已超时出队（或未知）的回应
    return;
  }
  // 跳号：前面的请求丢了，按超时出队
  while (!pending_.empty() && pending_.front().first < seq) timeout_one();
  if (!pending_.empty() && pending_.front().first == seq) {
    success_one();
  } else {
    ++stats_.late;
  }
}

void RescueTracker::check(int64_t now_us) {
  while (!pending_.empty() && now_us - pending_.front().second > timeout_us_) timeout_one();
}

bool RescueTracker::settle() {
  const bool ok = batch_timeouts_ == 0;
  batch_timeouts_ = 0;
  (ok ? stats_.batches_ok : stats_.batches_fail) += 1;
  return ok;
}

bool RescueTarget::reached(double prob, double target) {
  return prob >= target * (1 - kReachTolerance);
}

double RescueTarget::target_for(int k, int n, double epsilon_factor) {
  if (n < 1) n = 1;
  const double eps = epsilon_factor / n;
  return std::max(0.0, 1.0 / n - eps * (std::pow(2.0, k) - 1));
}

double RescueTarget::on_batch(bool ok, int n) {
  k_ = ok ? 0 : k_ + 1;
  return target_for(k_, n, epsilon_factor_);
}

double Backoff::level_at(int64_t now_ms, const RescueParams& p) const {
  if (level_ <= 0) return 0;
  const int64_t dt = std::max<int64_t>(0, now_ms - level_ts_);
  return level_ * std::pow(0.5, static_cast<double>(dt) / p.backoff_half_life_ms);
}

int64_t Backoff::trigger(int64_t now_ms, const RescueParams& p) {
  level_ = level_at(now_ms, p);
  const double mult = std::pow(2.0, std::floor(level_ + 1e-9));
  const int64_t interval = static_cast<int64_t>(
      std::min(static_cast<double>(p.backoff_max_ms), static_cast<double>(p.backoff_min_ms) * mult));
  next_ms_ = now_ms + interval;
  level_ += 1;
  // 从退避结束时刻开始衰减：退避窗口内不衰减，否则窗口内的衰减会让下一次的 floor 落后一档
  level_ts_ = next_ms_;
  return interval;
}

}  // namespace lm::manager
