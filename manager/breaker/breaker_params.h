#pragma once

// 节点状态机、低频探测、急救的参数（manager.yaml 的 breaker 段），缺省值取开发文档“默认参数”表。

#include <cstdint>

#include "lm/config.h"

namespace lm::manager {

struct RescueParams {
  double min_score = 0.6;
  double prob_factor = 0.5;  // p < prob_factor / N 才急救
  int64_t interval_ms = 1;
  int64_t timeout_ms = 10;
  int64_t batch_ms = 100;
  int64_t max_ms = 10 * 1000;
  double t_decay = 0.9;
  double epsilon_factor = 0.125;  // ε = epsilon_factor / N
  int64_t backoff_min_ms = 300 * 1000;
  int64_t backoff_max_ms = 3600 * 1000;
  double backoff_half_life_ms = 86400.0 * 1000;
};

struct BreakerParams {
  int64_t slow_start_ms = 300 * 1000;
  double slow_start_from = 0.2;
  int recover_reports = 5;
  int64_t probe_interval_ms = 1000;
  int64_t probe_timeout_ms = 500;
  int probe_fail_evict = 3;
  int64_t gray_window_ms = 300 * 1000;
  int gray_min_samples = 20;
  double gray_factor = 3;
  double gray_min_ratio = 0.2;
  RescueParams rescue;

  static BreakerParams from_config(const lm::Config& cfg);  // 不合法抛 lm::ConfigError
  void validate() const;
};

}  // namespace lm::manager
