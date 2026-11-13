#include "runq_histogram.h"

#include <algorithm>

namespace lm::worker {

double runq_percentile(const std::array<uint64_t, RUNQ_BUCKETS>& delta, double p) {
  uint64_t n = 0;
  for (uint64_t c : delta) n += c;
  if (n == 0) return 0.0;
  const double rank = p * static_cast<double>(n);
  uint64_t cum = 0;
  for (unsigned b = 0; b < RUNQ_BUCKETS; ++b) {
    const uint64_t c = delta[b];
    if (c == 0) continue;
    if (static_cast<double>(cum + c) >= rank) {
      const double frac = (rank - static_cast<double>(cum)) / static_cast<double>(c);
      return static_cast<double>(runq_bucket_low(b)) +
             std::clamp(frac, 0.0, 1.0) * static_cast<double>(runq_bucket_width(b));
    }
    cum += c;
  }
  // 浮点误差导致没命中时取最后一个非空桶的上界
  for (int b = RUNQ_BUCKETS - 1; b >= 0; --b) {
    if (delta[static_cast<unsigned>(b)] != 0) {
      return static_cast<double>(runq_bucket_low(static_cast<unsigned>(b)) +
                                 runq_bucket_width(static_cast<unsigned>(b)));
    }
  }
  return 0.0;
}

double runq_drop_rate(uint64_t drops, uint64_t samples) {
  const uint64_t den = drops + samples;
  if (den == 0) return 0.0;
  return std::clamp(static_cast<double>(drops) / static_cast<double>(den), 0.0, 1.0);
}

std::optional<RunqStats> RunqWindow::update(const RunqCumulative& cur) {
  if (!has_prev_) {
    prev_ = cur;
    has_prev_ = true;
    return std::nullopt;
  }
  bool went_back = cur.drops < prev_.drops;
  for (unsigned b = 0; b < RUNQ_BUCKETS && !went_back; ++b) {
    went_back = cur.buckets[b] < prev_.buckets[b];
  }
  if (went_back) {
    prev_ = cur;
    return std::nullopt;
  }

  RunqStats s;
  for (unsigned b = 0; b < RUNQ_BUCKETS; ++b) {
    delta_[b] = cur.buckets[b] - prev_.buckets[b];
    s.samples += delta_[b];
  }
  s.drops = cur.drops - prev_.drops;
  s.p50_us = static_cast<float>(runq_percentile(delta_, 0.50));
  s.p99_us = static_cast<float>(runq_percentile(delta_, 0.99));
  s.drop_rate = static_cast<float>(runq_drop_rate(s.drops, s.samples));
  prev_ = cur;
  return s;
}

}  // namespace lm::worker
