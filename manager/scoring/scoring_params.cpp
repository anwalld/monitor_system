#include "scoring_params.h"

#include <cmath>
#include <string>

namespace lm::manager {

namespace {

[[noreturn]] void fail(const std::string& key, const std::string& why) {
  throw lm::ConfigError("配置项 " + key + " 不合法：" + why);
}

void read_range(const lm::Config& cfg, const std::string& prefix, Range* r) {
  r->good = cfg.get<double>(prefix + ".good", r->good);
  r->bad = cfg.get<double>(prefix + ".bad", r->bad);
}

void read_metric(const lm::Config& cfg, const std::string& prefix, MetricParams* m) {
  read_range(cfg, prefix, &m->range);
  m->evict = cfg.get<double>(prefix + ".evict", m->evict);
  m->recover = cfg.get<double>(prefix + ".recover", m->recover);
  m->weight = cfg.get<double>(prefix + ".weight", m->weight);
}

void check_range(const std::string& key, const Range& r) {
  if (!std::isfinite(r.good) || !std::isfinite(r.bad)) fail(key, "好点和坏点必须是有限数");
  if (r.good == r.bad) fail(key, "好点与坏点不能相等");
}

void check_metric(const std::string& key, const MetricParams& m) {
  check_range(key, m.range);
  if (!std::isfinite(m.evict) || !std::isfinite(m.recover)) fail(key, "剔除线和恢复线必须是有限数");
  if (m.higher_is_worse()) {
    if (m.evict < m.range.bad) fail(key + ".evict", "越大越坏的指标，剔除线不能低于坏点");
    if (m.recover >= m.evict) fail(key + ".recover", "恢复线必须低于剔除线");
  } else {
    if (m.evict > m.range.bad) fail(key + ".evict", "越小越坏的指标，剔除线不能高于坏点");
    if (m.recover <= m.evict) fail(key + ".recover", "恢复线必须高于剔除线");
  }
  if (!(m.weight >= 0)) fail(key + ".weight", "权重不能为负");
}

}  // namespace

void ScoringParams::validate() const {
  check_metric("scoring.cpu", cpu);
  check_metric("scoring.runq", runq);
  if (runq.range.good <= 0 || runq.range.bad <= 0) fail("scoring.runq", "对数映射要求好点和坏点大于 0");
  check_metric("scoring.mem", mem);
  check_range("scoring.net.rtt_ms", net.rtt_ms);
  check_range("scoring.net.timeout", net.timeout);
  check_range("scoring.net.retrans", net.retrans);
  if (!(net.evict > 0 && net.evict <= 1)) fail("scoring.net.evict", "必须在 (0, 1] 内");
  if (!(net.recover >= 0 && net.recover < net.evict)) fail("scoring.net.recover", "必须在 [0, evict) 内");
  if (!(net.weight >= 0)) fail("scoring.net.weight", "权重不能为负");
  if (!(cpu.weight + runq.weight + mem.weight + net.weight > 0)) fail("scoring", "权重之和必须大于 0");
  if (!(missing_score >= 0 && missing_score <= 1)) fail("scoring.missing_score", "必须在 [0, 1] 内");
  if (lost_after_ms <= 0) fail("scoring.lost_after_ms", "必须大于 0");
  if (evict_after < 1) fail("scoring.evict_after", "不能小于 1");
  if (recover_after < 1) fail("scoring.recover_after", "不能小于 1");
  if (!(immediate_factor > 1)) fail("scoring.immediate_factor", "必须大于 1");
}

ScoringParams ScoringParams::from_config(const lm::Config& cfg) {
  ScoringParams p;
  read_metric(cfg, "scoring.cpu", &p.cpu);
  read_metric(cfg, "scoring.runq", &p.runq);
  read_metric(cfg, "scoring.mem", &p.mem);
  read_range(cfg, "scoring.net.rtt_ms", &p.net.rtt_ms);
  read_range(cfg, "scoring.net.timeout", &p.net.timeout);
  read_range(cfg, "scoring.net.retrans", &p.net.retrans);
  p.net.evict = cfg.get<double>("scoring.net.evict", p.net.evict);
  p.net.recover = cfg.get<double>("scoring.net.recover", p.net.recover);
  p.net.weight = cfg.get<double>("scoring.net.weight", p.net.weight);
  p.missing_score = cfg.get<double>("scoring.missing_score", p.missing_score);
  p.lost_after_ms = cfg.get<int64_t>("scoring.lost_after_ms", p.lost_after_ms);
  p.evict_after = cfg.get<int>("scoring.evict_after", p.evict_after);
  p.recover_after = cfg.get<int>("scoring.recover_after", p.recover_after);
  p.immediate_factor = cfg.get<double>("scoring.immediate_factor", p.immediate_factor);
  p.validate();
  return p;
}

}  // namespace lm::manager
