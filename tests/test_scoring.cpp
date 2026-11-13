// 评分、剔除、抽样的纯逻辑测试（manager/scoring）。

#include <gtest/gtest.h>

#include <cmath>
#include <limits>
#include <random>

#include "scoring/eviction.h"
#include "scoring/score.h"
#include "scoring/scoring_params.h"
#include "scoring/selector.h"

using namespace lm::manager;

namespace {

lm::NicStat* add_nic(lm::Sample* s, const std::string& name, float rtt, float timeout,
                     float retrans) {
  auto* n = s->add_nics();
  n->set_name(name);
  n->set_rtt_ms(rtt);
  n->set_timeout_rate(timeout);
  n->set_retrans_rate(retrans);
  return n;
}

lm::Sample perfect() {
  lm::Sample s;
  s.set_node_id("w1");
  s.set_cpu_busy(0.1F);
  s.set_runq_p99_us(50.0F);
  s.set_mem_avail_ratio(0.8F);
  add_nic(&s, "ens160", 1.0F, 0.0F, 0.0F);
  return s;
}

// ---------- 参数 ----------

TEST(ScoringParams, DefaultsMatchDevDoc) {
  const auto p = ScoringParams::from_config(lm::Config::load_string("{}"));
  EXPECT_DOUBLE_EQ(p.cpu.range.good, 0.2);
  EXPECT_DOUBLE_EQ(p.cpu.range.bad, 0.9);
  EXPECT_DOUBLE_EQ(p.cpu.evict, 0.97);
  EXPECT_DOUBLE_EQ(p.cpu.recover, 0.85);
  EXPECT_DOUBLE_EQ(p.runq.evict, 20000);
  EXPECT_DOUBLE_EQ(p.mem.evict, 0.05);
  EXPECT_DOUBLE_EQ(p.net.rtt_ms.bad, 100);
  EXPECT_DOUBLE_EQ(p.net.weight, 0.2);
  EXPECT_DOUBLE_EQ(p.missing_score, 0.3);
  EXPECT_EQ(p.lost_after_ms, 3000);
  EXPECT_EQ(p.evict_after, 3);
  EXPECT_EQ(p.recover_after, 5);
}

TEST(ScoringParams, PartialOverride) {
  const auto p = ScoringParams::from_config(lm::Config::load_string(
      "scoring: {cpu: {weight: 0.5}, net: {rtt_ms: {bad: 200}}, lost_after_ms: 5000}"));
  EXPECT_DOUBLE_EQ(p.cpu.weight, 0.5);
  EXPECT_DOUBLE_EQ(p.cpu.range.bad, 0.9);
  EXPECT_DOUBLE_EQ(p.net.rtt_ms.bad, 200);
  EXPECT_EQ(p.lost_after_ms, 5000);
}

TEST(ScoringParams, ManagerYamlParses) {
  const auto p = ScoringParams::from_config(lm::Config::load_file(LM_CONFIG_DIR "/manager.yaml"));
  EXPECT_DOUBLE_EQ(p.runq.range.bad, 2000);
}

TEST(ScoringParams, InvalidValuesNameTheKey) {
  struct Case {
    const char* yaml;
    const char* key;
  } cases[] = {
      {"scoring: {cpu: {good: 0.5, bad: 0.5}}", "scoring.cpu"},
      {"scoring: {cpu: {evict: 0.8}}", "scoring.cpu.evict"},
      {"scoring: {cpu: {recover: 0.98}}", "scoring.cpu.recover"},
      {"scoring: {mem: {recover: 0.04}}", "scoring.mem.recover"},
      {"scoring: {runq: {good: 0}}", "scoring.runq"},
      {"scoring: {mem: {weight: -1}}", "scoring.mem.weight"},
      {"scoring: {cpu: {weight: 0}, runq: {weight: 0}, mem: {weight: 0}, net: {weight: 0}}",
       "scoring"},
      {"scoring: {net: {recover: 0.6}}", "scoring.net.recover"},
      {"scoring: {missing_score: 2}", "scoring.missing_score"},
      {"scoring: {evict_after: 0}", "scoring.evict_after"},
      {"scoring: {lost_after_ms: 0}", "scoring.lost_after_ms"},
  };
  for (const auto& c : cases) {
    try {
      ScoringParams::from_config(lm::Config::load_string(c.yaml));
      ADD_FAILURE() << "应当报错: " << c.yaml;
    } catch (const lm::ConfigError& e) {
      EXPECT_NE(std::string(e.what()).find(c.key), std::string::npos) << e.what();
    }
  }
}

// ---------- 归一化 ----------

TEST(Normalize, LinearAndClamp) {
  const Range cpu{0.2, 0.9};
  const double xs[] = {0.1, 0.2, 0.55, 0.9, 1.0};
  const double want[] = {1, 1, 0.5, 0, 0};
  for (int i = 0; i < 5; ++i) EXPECT_NEAR(normalize(xs[i], cpu), want[i], 1e-9) << xs[i];
}

TEST(Normalize, HigherIsBetter) {
  EXPECT_NEAR(normalize(0.3, Range{0.5, 0.1}), 0.5, 1e-9);
  EXPECT_NEAR(normalize(0.6, Range{0.5, 0.1}), 1, 1e-9);
  EXPECT_NEAR(normalize(0.05, Range{0.5, 0.1}), 0, 1e-9);
}

TEST(Normalize, Log) {
  const Range r{100, 2000};
  EXPECT_NEAR(normalize_log(std::sqrt(100.0 * 2000.0), r), 0.5, 1e-3);
  EXPECT_NEAR(normalize_log(447.2, r), 0.5, 1e-3);
  EXPECT_NEAR(normalize_log(50, r), 1, 1e-9);
  EXPECT_NEAR(normalize_log(5000, r), 0, 1e-9);
  EXPECT_NEAR(normalize_log(0, r), 1, 1e-9);
}

TEST(Normalize, NonFiniteIsZero) {
  const double nan = std::numeric_limits<double>::quiet_NaN();
  EXPECT_EQ(normalize(nan, Range{0.2, 0.9}), 0);
  EXPECT_EQ(normalize(INFINITY, Range{0.2, 0.9}), 0);
  EXPECT_EQ(normalize_log(nan, Range{100, 2000}), 0);
  lm::Sample s = perfect();
  s.set_cpu_busy(std::numeric_limits<float>::quiet_NaN());
  const auto b = compute_score(s, ScoringParams{});
  EXPECT_EQ(b.cpu, 0);
  EXPECT_EQ(b.invalid, 1);
}

// ---------- 网络分与总分 ----------

TEST(Score, NicScoreIsMinOfThree) {
  const NetParams p;
  lm::Sample s;
  EXPECT_NEAR(nic_score(*add_nic(&s, "a", 52.5F, 0, 0), p), 0.5, 1e-6);
  EXPECT_NEAR(nic_score(*add_nic(&s, "b", 5.0F, 0.05F, 0), p), 0.5, 1e-6);
  EXPECT_NEAR(nic_score(*add_nic(&s, "c", 1.0F, 0, 0.0375F), p), 0.25, 1e-6);
}

TEST(Score, NetTakesBestNic) {
  lm::Sample s = perfect();
  s.clear_nics();
  add_nic(&s, "ens160", 81.0F, 0, 0);  // 0.2
  add_nic(&s, "ens192", 14.5F, 0, 0);  // 0.9
  const auto b = compute_score(s, ScoringParams{});
  EXPECT_NEAR(b.net, 0.9, 1e-6);
  EXPECT_EQ(b.best_nic, "ens192");
}

TEST(Score, AllPerfect) {
  const auto b = compute_score(perfect(), ScoringParams{});
  EXPECT_NEAR(b.total, 1, 1e-9);
  EXPECT_FALSE(b.runq_missing);
  EXPECT_FALSE(b.net_missing);
  EXPECT_EQ(b.best_nic, "ens160");
}

TEST(Score, MissingRunqCountsAs03) {
  lm::Sample s = perfect();
  s.clear_runq_p99_us();
  const auto b = compute_score(s, ScoringParams{});
  EXPECT_TRUE(b.runq_missing);
  EXPECT_NEAR(b.runq, 0.3, 1e-9);
  EXPECT_NEAR(b.total, 0.79, 1e-9);
}

TEST(Score, MissingNicsCountsAs03) {
  lm::Sample s = perfect();
  s.clear_nics();
  const auto b = compute_score(s, ScoringParams{});
  EXPECT_TRUE(b.net_missing);
  EXPECT_EQ(b.best_nic, "");
  EXPECT_NEAR(b.total, 0.8 + 0.2 * 0.3, 1e-9);
}

TEST(Score, RunqZeroIsPresentNotMissing) {
  lm::Sample s = perfect();
  s.set_runq_p99_us(0.0F);
  const auto b = compute_score(s, ScoringParams{});
  EXPECT_FALSE(b.runq_missing);
  EXPECT_NEAR(b.runq, 1, 1e-9);
}

// ---------- 剔除 ----------

GateParams cpu_gate() { return GateParams{0.97, 0.85, true, 3, 5, 10}; }

TEST(MetricGate, ThreeInARowRequired) {
  MetricGate g;
  for (double x : {0.98, 0.98, 0.5, 0.98, 0.98}) {
    g.update(x, cpu_gate());
    EXPECT_FALSE(g.evicted()) << x;
  }
  g.update(0.98, cpu_gate());
  EXPECT_TRUE(g.evicted());
}

TEST(MetricGate, ImmediateAtTenTimes) {
  MetricGate g;
  const GateParams runq{20000, 5000, true, 3, 5, 10};
  g.update(199999, runq);
  EXPECT_FALSE(g.evicted());
  g.update(250000, runq);
  EXPECT_TRUE(g.evicted());
}

TEST(MetricGate, HysteresisRecovery) {
  MetricGate g;
  for (int i = 0; i < 3; ++i) g.update(0.98, cpu_gate());
  ASSERT_TRUE(g.evicted());
  const double xs[] = {0.8, 0.8, 0.9, 0.8, 0.8, 0.8, 0.8};
  for (double x : xs) {
    g.update(x, cpu_gate());
    EXPECT_TRUE(g.evicted()) << x;
  }
  EXPECT_EQ(g.good_streak(), 4);
  g.update(0.8, cpu_gate());
  EXPECT_FALSE(g.evicted());
}

TEST(MetricGate, LowerIsWorseMemory) {
  MetricGate g;
  const GateParams mem{0.05, 0.1, false, 3, 5, 10};
  g.update(0.04, mem);
  g.update(0.04, mem);
  EXPECT_FALSE(g.evicted());
  g.update(0.04, mem);
  EXPECT_TRUE(g.evicted());
  for (int i = 0; i < 4; ++i) g.update(0.1, mem);
  EXPECT_TRUE(g.evicted());
  g.update(0.12, mem);
  EXPECT_FALSE(g.evicted());

  MetricGate h;
  h.update(0.004, mem);  // ≤ 0.05 / 10
  EXPECT_TRUE(h.evicted());
}

TEST(MetricGate, NonFiniteKeepsState) {
  MetricGate g;
  g.update(0.98, cpu_gate());
  g.update(std::numeric_limits<double>::quiet_NaN(), cpu_gate());
  EXPECT_EQ(g.bad_streak(), 1);
}

TEST(EvictionTracker, CpuReasonAndMissingRunqKeepsEvicted) {
  const ScoringParams p;
  EvictionTracker t;
  lm::Sample s = perfect();
  s.set_cpu_busy(0.98F);
  t.update(s, p);
  t.update(s, p);
  EXPECT_FALSE(t.evicted());
  t.update(s, p);
  EXPECT_TRUE(t.evicted());
  EXPECT_EQ(t.reason(), "cpu");

  EvictionTracker r;
  lm::Sample slow = perfect();
  slow.set_runq_p99_us(250000.0F);
  r.update(slow, p);
  EXPECT_EQ(r.reason(), "runq");
  lm::Sample no_runq = perfect();
  no_runq.clear_runq_p99_us();
  for (int i = 0; i < 10; ++i) r.update(no_runq, p);
  EXPECT_TRUE(r.evicted());
}

TEST(EvictionTracker, NetUsesMinTimeoutAcrossNics) {
  const ScoringParams p;
  EvictionTracker t;
  lm::Sample s = perfect();
  s.clear_nics();
  add_nic(&s, "ens160", 1, 0.9F, 0);
  add_nic(&s, "ens192", 1, 0.4F, 0);  // 一块网卡还行：不越线
  for (int i = 0; i < 5; ++i) t.update(s, p);
  EXPECT_FALSE(t.evicted());
  s.mutable_nics(1)->set_timeout_rate(0.6F);
  for (int i = 0; i < 3; ++i) t.update(s, p);
  EXPECT_EQ(t.reason(), "net");
  // 多个指标同时剔除
  s.set_cpu_busy(0.99F);
  for (int i = 0; i < 3; ++i) t.update(s, p);
  EXPECT_EQ(t.reason(), "cpu,net");
}

// ---------- 抽样 ----------

TEST(Selector, Boundaries) {
  const std::vector<double> prefix = {1, 1, 2};  // 权重 1、0、1
  const double us[] = {0, 0.4999, 0.5, 0.9999};
  const std::size_t want[] = {0, 0, 2, 2};
  for (int i = 0; i < 4; ++i) {
    auto r = pick_weighted(prefix, us[i]);
    ASSERT_TRUE(r);
    EXPECT_EQ(*r, want[i]) << us[i];
  }
  EXPECT_EQ(*pick_weighted(prefix, 1.0), 2U);  // u 越界被截断
  EXPECT_EQ(*pick_weighted({1, 1}, 0.99999999), 0U);  // 末尾权重为 0 时不会选中
}

TEST(Selector, EmptyOrZero) {
  EXPECT_FALSE(pick_weighted({}, 0.5));
  EXPECT_FALSE(pick_weighted({0, 0}, 0.5));
}

TEST(Selector, WeightFactors) {
  EXPECT_DOUBLE_EQ(weight(0.8, Factors{}), 0.8);
  EXPECT_DOUBLE_EQ(weight(0.8, Factors{0.5, 1, 0.2, 1}), 0.08);
  EXPECT_EQ(weight(-1, Factors{}), 0);
}

TEST(Selector, FrequencyMatchesWeights) {
  const std::vector<double> w = {0.2, 0.3, 0.5};
  std::vector<double> prefix;
  double acc = 0;
  for (double x : w) prefix.push_back(acc += x);
  std::mt19937_64 rng(42);
  std::uniform_real_distribution<double> uni(0.0, 1.0);
  int count[3] = {0, 0, 0};
  const int n = 100000;
  for (int i = 0; i < n; ++i) ++count[*pick_weighted(prefix, uni(rng))];
  for (int i = 0; i < 3; ++i) {
    EXPECT_NEAR(static_cast<double>(count[i]) / n, w[i], 0.01) << i;
  }
}

}  // namespace
