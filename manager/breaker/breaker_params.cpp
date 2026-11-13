#include "breaker_params.h"

#include <string>

namespace lm::manager {

namespace {

[[noreturn]] void fail(const std::string& key, const std::string& why) {
  throw lm::ConfigError("配置项 " + key + " 不合法：" + why);
}

int64_t ms(const lm::Config& cfg, const std::string& key, int64_t def_ms, double unit_ms) {
  return static_cast<int64_t>(cfg.get<double>(key, static_cast<double>(def_ms) / unit_ms) * unit_ms);
}

}  // namespace

void BreakerParams::validate() const {
  if (slow_start_ms <= 0) fail("breaker.slow_start_s", "必须大于 0");
  if (!(slow_start_from > 0 && slow_start_from <= 1)) fail("breaker.slow_start_from", "必须在 (0, 1] 内");
  if (recover_reports < 1) fail("breaker.recover_reports", "不能小于 1");
  if (probe_interval_ms < 100) fail("breaker.probe_interval_ms", "不能小于 100");
  if (probe_timeout_ms <= 0 || probe_timeout_ms > probe_interval_ms) {
    fail("breaker.probe_timeout_ms", "必须大于 0 且不超过 probe_interval_ms");
  }
  if (probe_fail_evict < 1) fail("breaker.probe_fail_evict", "不能小于 1");
  if (gray_window_ms <= 0) fail("breaker.gray_window_s", "必须大于 0");
  if (gray_min_samples < 1) fail("breaker.gray_min_samples", "不能小于 1");
  if (!(gray_factor >= 1)) fail("breaker.gray_factor", "不能小于 1");
  if (!(gray_min_ratio >= 0 && gray_min_ratio <= 1)) fail("breaker.gray_min_ratio", "必须在 [0, 1] 内");
  const RescueParams& r = rescue;
  if (!(r.min_score >= 0 && r.min_score <= 1)) fail("breaker.rescue.min_score", "必须在 [0, 1] 内");
  if (!(r.prob_factor > 0 && r.prob_factor <= 1)) fail("breaker.rescue.prob_factor", "必须在 (0, 1] 内");
  if (r.interval_ms < 1) fail("breaker.rescue.interval_ms", "不能小于 1");
  if (r.timeout_ms < r.interval_ms) fail("breaker.rescue.timeout_ms", "不能小于 interval_ms");
  if (r.batch_ms < r.timeout_ms) fail("breaker.rescue.batch_ms", "不能小于 timeout_ms");
  if (r.max_ms < r.batch_ms) fail("breaker.rescue.max_s", "不能短于一批");
  if (!(r.t_decay > 0 && r.t_decay < 1)) fail("breaker.rescue.t_decay", "必须在 (0, 1) 内");
  if (!(r.epsilon_factor > 0)) fail("breaker.rescue.epsilon_factor", "必须大于 0");
  if (r.backoff_min_ms <= 0) fail("breaker.rescue.backoff_min_s", "必须大于 0");
  if (r.backoff_max_ms < r.backoff_min_ms) fail("breaker.rescue.backoff_max_s", "不能小于 backoff_min_s");
  if (!(r.backoff_half_life_ms > 0)) fail("breaker.rescue.backoff_half_life_s", "必须大于 0");
}

BreakerParams BreakerParams::from_config(const lm::Config& cfg) {
  BreakerParams p;
  p.slow_start_ms = ms(cfg, "breaker.slow_start_s", p.slow_start_ms, 1000);
  p.slow_start_from = cfg.get<double>("breaker.slow_start_from", p.slow_start_from);
  p.recover_reports = cfg.get<int>("breaker.recover_reports", p.recover_reports);
  p.probe_interval_ms = ms(cfg, "breaker.probe_interval_ms", p.probe_interval_ms, 1);
  p.probe_timeout_ms = ms(cfg, "breaker.probe_timeout_ms", p.probe_timeout_ms, 1);
  p.probe_fail_evict = cfg.get<int>("breaker.probe_fail_evict", p.probe_fail_evict);
  p.gray_window_ms = ms(cfg, "breaker.gray_window_s", p.gray_window_ms, 1000);
  p.gray_min_samples = cfg.get<int>("breaker.gray_min_samples", p.gray_min_samples);
  p.gray_factor = cfg.get<double>("breaker.gray_factor", p.gray_factor);
  p.gray_min_ratio = cfg.get<double>("breaker.gray_min_ratio", p.gray_min_ratio);
  RescueParams& r = p.rescue;
  r.min_score = cfg.get<double>("breaker.rescue.min_score", r.min_score);
  r.prob_factor = cfg.get<double>("breaker.rescue.prob_factor", r.prob_factor);
  r.interval_ms = ms(cfg, "breaker.rescue.interval_ms", r.interval_ms, 1);
  r.timeout_ms = ms(cfg, "breaker.rescue.timeout_ms", r.timeout_ms, 1);
  r.batch_ms = ms(cfg, "breaker.rescue.batch_ms", r.batch_ms, 1);
  r.max_ms = ms(cfg, "breaker.rescue.max_s", r.max_ms, 1000);
  r.t_decay = cfg.get<double>("breaker.rescue.t_decay", r.t_decay);
  r.epsilon_factor = cfg.get<double>("breaker.rescue.epsilon_factor", r.epsilon_factor);
  r.backoff_min_ms = ms(cfg, "breaker.rescue.backoff_min_s", r.backoff_min_ms, 1000);
  r.backoff_max_ms = ms(cfg, "breaker.rescue.backoff_max_s", r.backoff_max_ms, 1000);
  r.backoff_half_life_ms =
      cfg.get<double>("breaker.rescue.backoff_half_life_s", r.backoff_half_life_ms / 1000) * 1000;
  p.validate();
  return p;
}

}  // namespace lm::manager
