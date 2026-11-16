#include <gtest/gtest.h>

#include <atomic>
#include <thread>
#include <vector>

#include "ingest/node_table.h"

using lm::manager::AcceptedSample;
using lm::manager::NodeTable;

namespace {

lm::Sample sample(const std::string& id, uint64_t seq, int ncpu = 4) {
  lm::Sample s;
  s.set_node_id(id);
  s.set_seq(seq);
  for (int i = 0; i < ncpu; ++i) s.add_core_busy(0.1F);
  return s;
}

TEST(NodeTable, RegistersNewNodeWithPeerAddrAndCores) {
  NodeTable t({});
  AcceptedSample a;
  ASSERT_EQ(t.apply(sample("w1", 1, 4), "10.0.0.11", 1000, 5000, &a), NodeTable::Result::kAccepted);
  EXPECT_TRUE(a.new_node);
  auto n = t.get("w1");
  ASSERT_TRUE(n);
  EXPECT_EQ(n->addr, "10.0.0.11:50052");
  EXPECT_EQ(n->ncpu, 4U);
  EXPECT_EQ(n->first_seen_wall_ms, 5000);
}

TEST(NodeTable, DropsOutOfOrderWithinRestartGap) {
  NodeTable t({});
  t.apply(sample("w1", 100), "10.0.0.11", 1000, 0, nullptr);
  EXPECT_EQ(t.apply(sample("w1", 99), "10.0.0.11", 1500, 0, nullptr), NodeTable::Result::kStale);
  EXPECT_EQ(t.apply(sample("w1", 100), "10.0.0.11", 1600, 0, nullptr), NodeTable::Result::kStale);
  EXPECT_EQ(t.get("w1")->last_seq, 100U);
}

TEST(NodeTable, AcceptsSeqResetAfterSilence) {
  NodeTable t({});
  t.apply(sample("w1", 100), "10.0.0.11", 1000, 0, nullptr);
  // 停 5 秒后重启，seq 从 1 开始
  EXPECT_EQ(t.apply(sample("w1", 1), "10.0.0.11", 6000, 0, nullptr), NodeTable::Result::kAccepted);
  EXPECT_EQ(t.get("w1")->last_seq, 1U);
}

TEST(NodeTable, RejectsBadNodeId) {
  NodeTable t({});
  EXPECT_EQ(t.apply(sample("", 1), "10.0.0.11", 0, 0, nullptr), NodeTable::Result::kInvalid);
  EXPECT_EQ(t.apply(sample(std::string(65, 'x'), 1), "10.0.0.11", 0, 0, nullptr),
            NodeTable::Result::kInvalid);
  EXPECT_EQ(t.apply(sample(std::string(64, 'x'), 1), "10.0.0.11", 0, 0, nullptr),
            NodeTable::Result::kAccepted);
  EXPECT_EQ(t.counts(0).total, 1U);
}

TEST(NodeTable, CountsOnlineWithinWindow) {
  NodeTable t({});
  t.apply(sample("w1", 1), "10.0.0.11", 1000, 0, nullptr);
  t.apply(sample("w2", 1), "10.0.0.12", 4000, 0, nullptr);
  auto c = t.counts(6000);  // w1 已 5 秒没上报
  EXPECT_EQ(c.total, 2U);
  EXPECT_EQ(c.online, 1U);
  EXPECT_EQ(*t.age_ms("w1", 6000), 5000);
  EXPECT_FALSE(t.age_ms("nope", 6000));
}

lm::Sample healthy_sample(const std::string& id, uint64_t seq) {
  lm::Sample s = sample(id, seq, 2);
  s.set_cpu_busy(0.1F);
  s.set_mem_avail_ratio(0.8F);
  s.set_runq_p99_us(50.0F);
  return s;
}

// M9：预测因子参与权重，过期回到 1，越界限幅，未知节点忽略
TEST(NodeTable, PredictionFactorAffectsWeightAndExpires) {
  NodeTable::Options o;
  o.breaker.slow_start_ms = 0;  // 直接进入正常，f_slow = 1
  NodeTable t(o);
  t.apply(healthy_sample("w1", 1), "10.0.0.11", 1000, 1000, nullptr);
  t.apply(healthy_sample("w2", 1), "10.0.0.12", 1000, 1000, nullptr);
  std::vector<lm::manager::StateChange> ch;
  std::vector<lm::manager::RescueStart> rs;
  t.tick_states(1100, 1100, &ch, &rs);
  const double before = t.select(1100, 0.0).pick->prob;
  EXPECT_NEAR(before, 0.5, 1e-9);

  auto r = t.set_predictions({{"w1", 0.9, 0.5}, {"w2", 0.2, 1.0}, {"nope", 0.5, 0.5}}, 0, 1200);
  EXPECT_EQ(r.applied, 2);
  EXPECT_EQ(r.unknown, 1);
  EXPECT_EQ(r.ttl_ms, 10000);  // ttl 为 0 用默认
  EXPECT_DOUBLE_EQ(*t.f_pred("w1", 1200), 0.5);
  EXPECT_DOUBLE_EQ(*t.f_pred("w2", 1200), 1.0);
  EXPECT_FALSE(t.f_pred("nope", 1200));
  // w1 权重减半：u=0 选中 w1（按 node_id 排序第一个），prob = 0.5/(0.5+1)
  EXPECT_NEAR(t.select(1200, 0.0).pick->prob, 1.0 / 3.0, 1e-9);
  // 过期后回到 1/2（先补一次上报，避免两节点因失联不在候选里）
  t.apply(healthy_sample("w1", 10), "10.0.0.11", 11000, 11000, nullptr);
  t.apply(healthy_sample("w2", 10), "10.0.0.12", 11000, 11000, nullptr);
  EXPECT_DOUBLE_EQ(*t.f_pred("w1", 11201), 1.0);
  const auto sel = t.select(11201, 0.0);
  ASSERT_TRUE(sel.pick);
  EXPECT_NEAR(sel.pick->prob, 0.5, 1e-9);

  // 限幅与 ttl 上限
  r = t.set_predictions({{"w1", 1.5, 0.1}, {"w2", -1.0, 1.5}}, 999999, 20000);
  EXPECT_EQ(r.ttl_ms, 60000);
  EXPECT_DOUBLE_EQ(*t.f_pred("w1", 20000), 0.5);
  EXPECT_DOUBLE_EQ(*t.f_pred("w2", 20000), 1.0);
  auto views = t.views(20000);
  ASSERT_EQ(views.size(), 2U);
  EXPECT_DOUBLE_EQ(views[0].f_pred, 0.5);
  ASSERT_TRUE(views[0].pred_cpu);
  EXPECT_DOUBLE_EQ(*views[0].pred_cpu, 1.0);  // cpu 限幅到 [0, 1]
  EXPECT_DOUBLE_EQ(*views[1].pred_cpu, 0.0);
  // 上报时带出 f_pred / pred_cpu；过期后 pred_cpu 为空
  AcceptedSample a;
  t.apply(healthy_sample("w1", 20), "10.0.0.11", 21000, 21000, &a);
  EXPECT_DOUBLE_EQ(a.f_pred, 0.5);
  EXPECT_TRUE(a.pred_cpu);
  t.apply(healthy_sample("w1", 30), "10.0.0.11", 90000, 90000, &a);
  EXPECT_DOUBLE_EQ(a.f_pred, 1.0);
  EXPECT_FALSE(a.pred_cpu);
}

TEST(NodeTable, PredictionIgnoredWhenAdvisorDisabled) {
  NodeTable::Options o;
  o.advisor.enabled = false;
  NodeTable t(o);
  t.apply(healthy_sample("w1", 1), "10.0.0.11", 1000, 1000, nullptr);
  auto r = t.set_predictions({{"w1", 0.9, 0.5}}, 10000, 1000);
  EXPECT_EQ(r.applied, 0);
  EXPECT_DOUBLE_EQ(*t.f_pred("w1", 1000), 1.0);
}

TEST(AdvisorParams, FromConfigAndValidate) {
  auto cfg = lm::Config::load_string("advisor: {enabled: false, min_factor: 0.6, max_ttl_ms: 30000}");
  auto p = lm::manager::AdvisorParams::from_config(cfg);
  EXPECT_FALSE(p.enabled);
  EXPECT_DOUBLE_EQ(p.min_factor, 0.6);
  EXPECT_EQ(p.max_ttl_ms, 30000);
  EXPECT_THROW(lm::manager::AdvisorParams::from_config(lm::Config::load_string("advisor: {min_factor: 0}")),
               lm::ConfigError);
  EXPECT_THROW(lm::manager::AdvisorParams::from_config(lm::Config::load_string("advisor: {max_ttl_ms: 1}")),
               lm::ConfigError);
}

TEST(PeerParsing, Ipv4Ipv6AndEncoded) {
  using lm::manager::parse_peer_ip;
  EXPECT_EQ(parse_peer_ip("ipv4:10.0.0.11:54321"), "10.0.0.11");
  EXPECT_EQ(parse_peer_ip("ipv6:[::1]:54321"), "::1");
  EXPECT_EQ(parse_peer_ip("ipv6:%5B::1%5D:54321"), "::1");
  EXPECT_EQ(lm::manager::join_host_port("::1", 50052), "[::1]:50052");
}

}  // namespace

// ---------- M5：评分、剔除与选择 ----------

namespace {

lm::Sample healthy(const std::string& id, uint64_t seq, float cpu = 0.1F) {
  lm::Sample s = sample(id, seq, 2);
  s.set_cpu_busy(cpu);
  s.set_mem_avail_ratio(0.8F);
  s.set_runq_p99_us(50.0F);
  auto* n = s.add_nics();
  n->set_name("ens160");
  n->set_rtt_ms(1.0F);
  return s;
}

TEST(NodeTableScoring, AcceptedSampleCarriesScoreAndHints) {
  NodeTable t({});
  AcceptedSample a;
  lm::Sample s = healthy("w1", 1);
  s.clear_runq_p99_us();
  t.apply(s, "10.0.0.11", 1000, 0, &a);
  EXPECT_NEAR(a.score.total, 0.79, 1e-6);
  EXPECT_TRUE(a.hint_runq_missing);
  EXPECT_FALSE(a.hint_net_missing);
  EXPECT_FALSE(a.evicted);
  s.set_seq(2);
  t.apply(s, "10.0.0.11", 2000, 0, &a);
  EXPECT_FALSE(a.hint_runq_missing);  // 只提示一次
  t.apply(healthy("w1", 3), "10.0.0.11", 3000, 0, &a);
  s.set_seq(4);
  t.apply(s, "10.0.0.11", 4000, 0, &a);
  EXPECT_TRUE(a.hint_runq_missing);  // 恢复后再次缺失，再提示
}

TEST(NodeTableScoring, EvictionReportedAndExcludedFromSelect) {
  NodeTable t({});
  AcceptedSample a;
  t.apply(healthy("w1", 1), "10.0.0.11", 1000, 0, &a);
  t.apply(healthy("w2", 1), "10.0.0.12", 1000, 0, &a);
  for (uint64_t i = 2; i <= 4; ++i) t.apply(healthy("w1", i, 0.98F), "10.0.0.11", 1000 + i, 0, &a);
  EXPECT_TRUE(a.evicted);
  EXPECT_TRUE(a.evicted_changed);
  EXPECT_EQ(a.evict_reason, "cpu");
  for (double u : {0.0, 0.3, 0.7, 0.999}) {
    const auto r = t.select(1500, u);
    ASSERT_TRUE(r.pick);
    EXPECT_EQ(r.pick->node_id, "w2");
    EXPECT_EQ(r.pick->addr, "10.0.0.12:50052");
    EXPECT_NEAR(r.pick->prob, 1.0, 1e-9);
    EXPECT_EQ(r.evicted, 1U);
    EXPECT_EQ(r.total, 2U);
  }
}

TEST(NodeTableScoring, LostNodeExcludedThenBack) {
  NodeTable t({});
  t.apply(healthy("w1", 1), "10.0.0.11", 1000, 0, nullptr);
  EXPECT_TRUE(t.select(4000, 0.5).pick);  // 恰好 3000 ms
  const auto lost = t.select(4001, 0.5);
  EXPECT_FALSE(lost.pick);
  EXPECT_EQ(lost.lost, 1U);
  t.apply(healthy("w1", 2), "10.0.0.11", 9000, 0, nullptr);
  EXPECT_TRUE(t.select(9001, 0.5).pick);
}

TEST(NodeTableScoring, SelectSortedByNodeIdAndWeighted) {
  NodeTable t({});
  // w_a：CPU 0.55 → s_cpu 0.5，总分 0.85；w_b 满分 1.0
  t.apply(healthy("w_b", 1), "10.0.0.2", 1000, 0, nullptr);
  t.apply(healthy("w_a", 1, 0.55F), "10.0.0.1", 1000, 0, nullptr);
  const double total = 0.85 + 1.0;
  auto r = t.select(1000, 0.0);
  EXPECT_EQ(r.pick->node_id, "w_a");
  EXPECT_NEAR(r.pick->prob, 0.85 / total, 1e-6);
  r = t.select(1000, 0.85 / total - 1e-6);
  EXPECT_EQ(r.pick->node_id, "w_a");
  r = t.select(1000, 0.85 / total + 1e-6);
  EXPECT_EQ(r.pick->node_id, "w_b");
  EXPECT_EQ(r.candidates, 2U);
}

TEST(NodeTableScoring, ConcurrentApplyAndSelect) {
  NodeTable t({});
  std::atomic<bool> stop{false};
  std::vector<std::thread> th;
  for (int k = 0; k < 3; ++k) {
    th.emplace_back([&, k] {
      const std::string id = "w" + std::to_string(k);
      for (uint64_t i = 1; i <= 2000; ++i) {
        t.apply(healthy(id, i, (i % 7 == 0) ? 0.99F : 0.3F), "10.0.0.1", 1000, 0, nullptr);
      }
    });
  }
  std::atomic<int> picks{0};
  th.emplace_back([&] {
    while (!stop.load()) {
      if (t.select(1000, 0.5).pick) picks.fetch_add(1);
    }
  });
  for (int k = 0; k < 3; ++k) th[k].join();
  stop.store(true);
  th.back().join();
  EXPECT_EQ(t.node_ids().size(), 3U);
}

}  // namespace

// ---------- M10：对照分配策略 ----------

namespace {

using lm::manager::Policy;
int g_seq = 0;

std::string pick(NodeTable& t, int64_t now, Policy p) {
  const auto r = t.allocate(now, 0.5, "t" + std::to_string(++g_seq), 0, p);
  return r.pick ? r.pick->node_id : "";
}

TEST(NodeTablePolicy, ParsePolicy) {
  EXPECT_EQ(lm::manager::parse_policy(""), Policy::kScore);
  EXPECT_EQ(lm::manager::parse_policy("score"), Policy::kScore);
  EXPECT_EQ(lm::manager::parse_policy("score_nopred"), Policy::kScoreNoPred);
  EXPECT_EQ(lm::manager::parse_policy("rr"), Policy::kRoundRobin);
  EXPECT_EQ(lm::manager::parse_policy("wrr"), Policy::kWeightedRR);
  EXPECT_FALSE(lm::manager::parse_policy("foo"));
  EXPECT_STREQ(lm::manager::policy_name(Policy::kWeightedRR), "wrr");
}

TEST(NodeTablePolicy, RoundRobinIgnoresScoreEviction) {
  NodeTable t({});
  AcceptedSample a;
  t.apply(healthy("w1", 1), "10.0.0.11", 1000, 0, nullptr);
  t.apply(healthy("w2", 1), "10.0.0.12", 1000, 0, nullptr);
  t.apply(healthy("w3", 1), "10.0.0.13", 1000, 0, nullptr);
  for (uint64_t i = 2; i <= 4; ++i) t.apply(healthy("w2", i, 0.98F), "10.0.0.12", 1000 + i, 0, &a);
  ASSERT_TRUE(a.evicted);  // 关键指标剔除，本系统不会选它
  std::vector<std::string> got;
  for (int i = 0; i < 6; ++i) got.push_back(pick(t, 1100, Policy::kRoundRobin));
  EXPECT_EQ(got, (std::vector<std::string>{"w1", "w2", "w3", "w1", "w2", "w3"}));
  for (int i = 0; i < 6; ++i) EXPECT_NE(pick(t, 1100, Policy::kScore), "w2");
  // 选中的任务同样记入在途账
  EXPECT_GE(t.inflight_totals().inflight, 6U);
}

TEST(NodeTablePolicy, RoundRobinSkipsLostAndProbeFailed) {
  NodeTable t({});
  t.apply(healthy("w1", 1), "10.0.0.11", 1000, 0, nullptr);
  t.apply(healthy("w2", 1), "10.0.0.12", 1000, 0, nullptr);
  t.apply(healthy("w3", 1), "10.0.0.13", 1000, 0, nullptr);
  t.apply(healthy("w1", 2), "10.0.0.11", 4500, 0, nullptr);
  t.apply(healthy("w3", 2), "10.0.0.13", 4500, 0, nullptr);
  for (int i = 0; i < 4; ++i) EXPECT_NE(pick(t, 4500, Policy::kRoundRobin), "w2");  // w2 失联
  const auto r = t.select(4500, 0.5, Policy::kRoundRobin);
  EXPECT_EQ(r.lost, 1U);
  EXPECT_EQ(r.candidates, 2U);
  EXPECT_NEAR(r.pick->prob, 0.5, 1e-9);
  // w3 低频探测连续失败到剔除线
  for (int i = 0; i < t.options().breaker.probe_fail_evict; ++i) t.on_probe("w3", false, 4600, 0);
  for (int i = 0; i < 4; ++i) EXPECT_EQ(pick(t, 4600, Policy::kRoundRobin), "w1");
  EXPECT_FALSE(t.select(9000, 0.5, Policy::kRoundRobin).pick);  // 全部失联
}

TEST(NodeTablePolicy, SmoothWeightedRoundRobinByCores) {
  NodeTable t({});
  lm::Sample s1 = healthy("w1", 1);
  for (int i = 0; i < 2; ++i) s1.add_core_busy(0.1F);  // 4 核
  t.apply(s1, "10.0.0.11", 1000, 0, nullptr);
  t.apply(healthy("w2", 1), "10.0.0.12", 1000, 0, nullptr);  // 2 核
  std::vector<std::string> got;
  for (int i = 0; i < 6; ++i) got.push_back(pick(t, 1100, Policy::kWeightedRR));
  EXPECT_EQ(got, (std::vector<std::string>{"w1", "w2", "w1", "w1", "w2", "w1"}));
  EXPECT_NEAR(t.select(1100, 0.5, Policy::kWeightedRR).pick->prob, 4.0 / 6.0, 1e-9);
}

TEST(NodeTablePolicy, ScoreNoPredIgnoresPrediction) {
  NodeTable::Options o;
  o.breaker.slow_start_ms = 0;
  NodeTable t(o);
  t.apply(healthy_sample("w1", 1), "10.0.0.11", 1000, 1000, nullptr);
  t.apply(healthy_sample("w2", 1), "10.0.0.12", 1000, 1000, nullptr);
  std::vector<lm::manager::StateChange> ch;
  std::vector<lm::manager::RescueStart> rs;
  t.tick_states(1100, 1100, &ch, &rs);
  t.set_predictions({{"w1", 0.9, 0.5}}, 10000, 1200);
  EXPECT_NEAR(t.select(1200, 0.0, Policy::kScore).pick->prob, 1.0 / 3.0, 1e-9);
  EXPECT_NEAR(t.select(1200, 0.0, Policy::kScoreNoPred).pick->prob, 0.5, 1e-9);
}

}  // namespace
