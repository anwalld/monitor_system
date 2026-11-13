// 节点状态机、急救判定逻辑、退避（模拟时钟）。

#include <gtest/gtest.h>

#include <atomic>
#include <map>
#include <cmath>
#include <thread>

#include "breaker/breaker_params.h"
#include "breaker/rescue_logic.h"
#include "ingest/node_table.h"

using namespace lm::manager;

namespace {

constexpr int64_t S = 1000;

// ---------- 参数 ----------

TEST(BreakerParams, DefaultsAndInvalid) {
  const auto p = BreakerParams::from_config(lm::Config::load_string("{}"));
  EXPECT_EQ(p.slow_start_ms, 300 * S);
  EXPECT_DOUBLE_EQ(p.slow_start_from, 0.2);
  EXPECT_EQ(p.probe_fail_evict, 3);
  EXPECT_EQ(p.rescue.timeout_ms, 10);
  EXPECT_EQ(p.rescue.max_ms, 10 * S);
  EXPECT_EQ(p.rescue.backoff_min_ms, 300 * S);
  const auto q = BreakerParams::from_config(
      lm::Config::load_string("breaker: {slow_start_s: 30, rescue: {max_s: 5}}"));
  EXPECT_EQ(q.slow_start_ms, 30 * S);
  EXPECT_EQ(q.rescue.max_ms, 5 * S);
  struct Case {
    const char* yaml;
    const char* key;
  } cases[] = {
      {"breaker: {slow_start_from: 0}", "breaker.slow_start_from"},
      {"breaker: {probe_timeout_ms: 2000}", "breaker.probe_timeout_ms"},
      {"breaker: {rescue: {t_decay: 1}}", "breaker.rescue.t_decay"},
      {"breaker: {rescue: {timeout_ms: 200}}", "breaker.rescue.batch_ms"},
      {"breaker: {rescue: {backoff_max_s: 10}}", "breaker.rescue.backoff_max_s"},
  };
  for (const auto& c : cases) {
    try {
      BreakerParams::from_config(lm::Config::load_string(c.yaml));
      ADD_FAILURE() << c.yaml;
    } catch (const lm::ConfigError& e) {
      EXPECT_NE(std::string(e.what()).find(c.key), std::string::npos) << e.what();
    }
  }
}

TEST(BreakerParams, ManagerYamlParses) {
  const auto p = BreakerParams::from_config(lm::Config::load_file(LM_CONFIG_DIR "/manager.yaml"));
  EXPECT_EQ(p.slow_start_ms, 300 * S);
}

// ---------- 急救判定 ----------

TEST(RescueTracker, OutageCountsAsOneEvent) {
  RescueTracker t(10'000);
  int64_t now = 0;
  for (uint64_t seq = 1; seq <= 400; ++seq, now += 1000) {
    t.on_send(seq, now);
    if (seq <= 100 || seq > 300) t.on_response(seq, now + 200);
    t.check(now + 500);
  }
  t.check(now + 20'000);
  EXPECT_EQ(t.stats().ok, 200U);
  EXPECT_EQ(t.stats().timeouts, 200U);
  EXPECT_EQ(t.stats().events, 1U);
  EXPECT_EQ(t.pending(), 0U);
}

TEST(RescueTracker, LateResponseDropped) {
  RescueTracker t(10'000);
  for (uint64_t s = 1; s <= 5; ++s) t.on_send(s, 0);
  t.on_response(1, 100);
  t.on_response(2, 100);
  t.check(11'000);  // 3、4、5 超时出队
  EXPECT_EQ(t.stats().timeouts, 3U);
  t.on_response(5, 12'000);
  EXPECT_EQ(t.stats().late, 1U);
  EXPECT_EQ(t.stats().ok, 2U);
}

TEST(RescueTracker, GapPopsEarlierAsTimeouts) {
  RescueTracker t(10'000);
  for (uint64_t s = 1; s <= 4; ++s) t.on_send(s, 0);
  t.on_response(3, 100);
  EXPECT_EQ(t.stats().timeouts, 2U);
  EXPECT_EQ(t.stats().ok, 1U);
  EXPECT_FALSE(t.in_outage());
  EXPECT_EQ(t.pending(), 1U);
}

TEST(RescueTracker, BatchSettle) {
  RescueTracker t(10'000);
  t.on_send(1, 0);
  t.on_response(1, 10);
  EXPECT_TRUE(t.settle());
  t.on_send(2, 0);
  t.check(20'000);
  EXPECT_FALSE(t.settle());
  EXPECT_TRUE(t.settle());  // 计数已清零
  EXPECT_EQ(t.stats().batches_ok, 2);
  EXPECT_EQ(t.stats().batches_fail, 1);
}

TEST(RescueTarget, LowersOnConsecutiveFailures) {
  RescueTarget g(0.125);
  EXPECT_NEAR(g.on_batch(false, 2), 0.4375, 1e-12);
  EXPECT_NEAR(g.on_batch(false, 2), 0.3125, 1e-12);
  EXPECT_NEAR(g.on_batch(false, 2), 0.0625, 1e-12);
  EXPECT_NEAR(g.on_batch(false, 2), 0.0, 1e-12);
  EXPECT_NEAR(g.on_batch(true, 2), 0.5, 1e-12);
  EXPECT_EQ(g.k(), 0);
}

TEST(Backoff, ExponentialAndCapped) {
  RescueParams p;
  Backoff b;
  int64_t now = 0;
  EXPECT_TRUE(b.allowed(now));
  const int64_t want_min[] = {5, 10, 20, 40, 60, 60};
  for (int64_t m : want_min) {
    ASSERT_TRUE(b.allowed(now));
    EXPECT_EQ(b.trigger(now, p), m * 60 * S);
    now = b.next_ms();
    EXPECT_FALSE(b.allowed(now - 1));
  }
}

TEST(Backoff, DecaysWithHalfLife) {
  RescueParams p;
  Backoff b;
  for (int i = 0; i < 3; ++i) b.trigger(b.next_ms(), p);
  const int64_t later = b.next_ms() + 2 * 86400 * S;
  EXPECT_NEAR(b.level_at(b.next_ms() + 2 * 86400 * S, p), 0.75, 1e-9);
  EXPECT_EQ(b.trigger(later, p), 5 * 60 * S);
}

// ---------- 状态机 ----------

lm::Sample healthy(const std::string& id, uint64_t seq, float cpu = 0.1F) {
  lm::Sample s;
  s.set_node_id(id);
  s.set_seq(seq);
  s.add_core_busy(cpu);
  s.set_cpu_busy(cpu);
  s.set_mem_avail_ratio(0.8F);
  s.set_runq_p99_us(50.0F);
  auto* n = s.add_nics();
  n->set_name("eth0");
  n->set_rtt_ms(1);
  return s;
}

struct Fixture {
  NodeTable t;
  std::vector<StateChange> changes;
  std::vector<RescueStart> rescues;
  std::map<std::string, uint64_t> seq;
  explicit Fixture(NodeTable::Options o = {}) : t(std::move(o)) {}
  void report(const std::string& id, int64_t now, float cpu = 0.1F) {
    t.apply(healthy(id, ++seq[id], cpu), "10.0.0." + std::to_string(id.back() - '0'), now, now,
            nullptr);
  }
  void tick(int64_t now) { t.tick_states(now, now, &changes, &rescues); }
  NodeMode mode(const std::string& id) { return *t.mode(id); }
  std::string reason(const std::string& id) { return t.get(id)->br.reason; }
  // 让节点每秒上报直到 until（含）
  void run(const std::vector<std::string>& ids, int64_t from, int64_t until) {
    for (int64_t now = from; now <= until; now += S) {
      for (const auto& id : ids) report(id, now);
      tick(now);
    }
  }
};

TEST(NodeStateMachine, NewNodeSlowStartLinearThenNormal) {
  Fixture f;
  f.report("w1", 0);
  EXPECT_EQ(f.mode("w1"), NodeMode::kSlowStart);
  EXPECT_NEAR(f.t.f_slow("w1", 0), 0.2, 1e-12);
  f.run({"w1"}, S, 150 * S);
  EXPECT_NEAR(f.t.f_slow("w1", 150 * S), 0.6, 1e-12);
  EXPECT_EQ(f.mode("w1"), NodeMode::kSlowStart);
  f.run({"w1"}, 151 * S, 300 * S);
  EXPECT_EQ(f.mode("w1"), NodeMode::kNormal);
  EXPECT_NEAR(f.t.f_slow("w1", 300 * S), 1.0, 1e-12);
}

TEST(NodeStateMachine, SlowStartDoesNotStackInflightPenalty) {
  Fixture f;
  f.report("w1", 0);
  for (int i = 0; i < 6; ++i) f.t.allocate(0, 0.5, "t" + std::to_string(i), 0);
  EXPECT_NEAR(f.t.select(0, 0.5).pick->weight, 0.2, 1e-9);  // 不乘 1/3
}

TEST(NodeStateMachine, SlowStartNodeGetsLessTraffic) {
  NodeTable::Options o;
  o.breaker.slow_start_ms = 300 * S;
  Fixture f(o);
  f.run({"w1"}, 0, 300 * S);  // w1 正常
  ASSERT_EQ(f.mode("w1"), NodeMode::kNormal);
  f.report("w2", 300 * S);  // w2 刚进入慢启动
  EXPECT_NEAR(f.t.select(300 * S, 0.99).pick->prob, 0.2 / 1.2, 1e-9);
}

TEST(NodeStateMachine, LostEvictedWithin3sAndNoRescue) {
  Fixture f;
  f.run({"w1", "w2"}, 0, 10 * S);
  f.run({"w2"}, 11 * S, 13 * S);  // w1 最后一次上报在 10 s
  EXPECT_NE(f.mode("w1"), NodeMode::kEvicted);
  f.report("w2", 13 * S + 100);
  f.tick(13 * S + 100);  // 3.1 s
  EXPECT_EQ(f.mode("w1"), NodeMode::kEvicted);
  EXPECT_EQ(f.reason("w1"), "lost");
  EXPECT_TRUE(f.rescues.empty());
  for (int i = 0; i < 20; ++i) EXPECT_EQ(f.t.select(13 * S + 100, i / 20.0).pick->node_id, "w2");
}

TEST(NodeStateMachine, RecoveryAfterFiveGoodReportsResetsLedger) {
  Fixture f;
  f.run({"w1", "w2"}, 0, 5 * S);
  // 给 w1 一点超时计数和基线
  f.t.allocate(5 * S, 0.0, "x", 0);
  f.t.on_event("x", lm::TaskEvent::RECEIVED, 5 * S);
  f.t.tick(305 * S, nullptr);
  f.t.tick_second(60);
  f.run({"w2"}, 6 * S, 10 * S);
  ASSERT_EQ(f.mode("w1"), NodeMode::kEvicted);
  for (int i = 0; i < 4; ++i) {
    f.report("w1", (11 + i) * S);
    f.tick((11 + i) * S);
    EXPECT_EQ(f.mode("w1"), NodeMode::kEvicted) << i;
  }
  f.report("w1", 15 * S);
  EXPECT_EQ(f.mode("w1"), NodeMode::kSlowStart);  // 第 5 条上报立即转换
  const auto snap = f.t.inflight_snapshot("w1");
  EXPECT_EQ(snap.timeout_count, 0);
}

TEST(NodeStateMachine, RecoveryInterruptedByMetric) {
  Fixture f;
  f.run({"w1"}, 0, 2 * S);
  f.t.tick_states(6 * S, 6 * S, nullptr, nullptr);  // 失联剔除
  ASSERT_EQ(f.mode("w1"), NodeMode::kEvicted);
  for (int i = 0; i < 4; ++i) f.report("w1", (7 + i) * S);
  f.report("w1", 11 * S, 0.98F);  // 越限：计数清零（关键指标尚未剔除，但仍然算“不正常”吗？）
  // 关键指标只越线一次还没有剔除，所以这一条仍算正常；连续 3 次越线后剔除并清零
  f.report("w1", 12 * S, 0.98F);
  f.report("w1", 13 * S, 0.98F);
  EXPECT_EQ(f.mode("w1"), NodeMode::kEvicted);
  EXPECT_EQ(f.t.get("w1")->br.good_reports, 0);
}

TEST(NodeStateMachine, ProbeFailuresEvict) {
  Fixture f;
  f.run({"w1"}, 0, 2 * S);
  EXPECT_FALSE(f.t.on_probe("w1", false, 2 * S, 0));
  EXPECT_FALSE(f.t.on_probe("w1", false, 2 * S, 0));
  auto c = f.t.on_probe("w1", false, 2 * S, 0);
  ASSERT_TRUE(c);
  EXPECT_EQ(c->to, NodeMode::kEvicted);
  EXPECT_EQ(c->reason, "probe");
  // 恢复：探测成功 + 5 条正常上报
  f.t.on_probe("w1", true, 3 * S, 0);
  f.run({"w1"}, 3 * S, 7 * S);
  EXPECT_EQ(f.mode("w1"), NodeMode::kSlowStart);
}

TEST(NodeStateMachine, UnconfirmedRatioEvicts) {
  NodeTable::Options o;
  o.inflight.ack_timeout_ms = 10 * S;
  Fixture f(o);
  f.run({"w1", "w2"}, 0, 1 * S);
  // w1：20 个样本，10 个未确认；w2：100 个确认
  int id = 0;
  auto alloc_to = [&](const std::string& node, bool confirm) {
    // 选中指定节点：u 取 0 选 w1（排序第一），取 0.999 选 w2
    const std::string tid = "t" + std::to_string(id++);
    auto r = f.t.allocate(1 * S, node == "w1" ? 0.0 : 0.999, tid, 0);
    EXPECT_EQ(r.pick->node_id, node);
    f.t.mark_sent(tid, 1 * S);
    if (confirm) f.t.on_event(tid, lm::TaskEvent::RECEIVED, 1 * S);
  };
  for (int i = 0; i < 20; ++i) alloc_to("w1", i % 2 == 0);
  for (int i = 0; i < 100; ++i) alloc_to("w2", true);
  f.t.tick(11 * S, nullptr);  // 10 个未确认
  f.report("w1", 11 * S);
  f.report("w2", 11 * S);
  f.tick(11 * S);
  EXPECT_EQ(f.mode("w1"), NodeMode::kEvicted);
  EXPECT_EQ(f.reason("w1"), "unconfirmed");
  EXPECT_NE(f.mode("w2"), NodeMode::kEvicted);
}

TEST(NodeStateMachine, TooFewSamplesNoGray) {
  NodeTable::Options o;
  o.inflight.ack_timeout_ms = 10 * S;
  Fixture f(o);
  f.run({"w1", "w2"}, 0, 1 * S);
  for (int i = 0; i < 19; ++i) {
    const std::string tid = "t" + std::to_string(i);
    f.t.allocate(1 * S, 0.0, tid, 0);
    f.t.mark_sent(tid, 1 * S);
  }
  f.t.tick(11 * S, nullptr);
  f.report("w1", 11 * S);
  f.report("w2", 11 * S);
  f.tick(11 * S);
  EXPECT_NE(f.mode("w1"), NodeMode::kEvicted);
}

// 让 w1 的 T 达到 k（执行超时 k 次）
void add_exec_timeouts(Fixture& f, int k, int64_t now) {
  for (int i = 0; i < k; ++i) {
    const std::string tid = "x" + std::to_string(i);
    f.t.allocate(now, 0.0, tid, 0);
    f.t.on_event(tid, lm::TaskEvent::RECEIVED, now);
  }
  f.t.tick(now + 300 * S, nullptr);
}

lm::Sample with_cpu_score(const std::string& id, uint64_t seq, double score) {
  // CPU 分数 s 使总分 = 0.3 s + 0.7（其余满分）；score < 0.7 时再压低内存
  lm::Sample s = healthy(id, seq);
  double cpu_s = (score - 0.7) / 0.3;
  if (cpu_s < 0) {
    s.set_cpu_busy(0.9F);
    const double mem_s = (score - 0.5) / 0.2;  // 0.3 + 0.2 mem_s + 0.3 + 0.2 ... 简化：cpu 0、runq 1、net 1
    s.set_mem_avail_ratio(static_cast<float>(0.1 + 0.4 * mem_s));
  } else {
    s.set_cpu_busy(static_cast<float>(0.9 - 0.7 * cpu_s));
  }
  return s;
}

TEST(NodeStateMachine, RescueTriggerAndScoreTooLow) {
  for (const double w1_score : {0.9, 0.5}) {
    NodeTable::Options o;
    o.breaker.slow_start_ms = 1;
    Fixture f(o);
    uint64_t q = 0;
    auto feed = [&](int64_t now) {
      f.t.apply(with_cpu_score("w1", ++q, w1_score), "10.0.0.1", now, now, nullptr);
      f.t.apply(with_cpu_score("w2", q, 0.65), "10.0.0.2", now, now, nullptr);
    };
    feed(0);
    f.tick(1);
    ASSERT_EQ(f.mode("w1"), NodeMode::kNormal);
    add_exec_timeouts(f, 4, 1);
    feed(301 * S + 1);
    f.tick(301 * S + 1);
    if (w1_score == 0.9) {
      ASSERT_EQ(f.rescues.size(), 1U);
      EXPECT_EQ(f.rescues[0].node_id, "w1");
      EXPECT_EQ(f.rescues[0].candidates, 2);
      EXPECT_NEAR(f.rescues[0].prob, 0.9 * std::exp(-2.0) / (0.9 * std::exp(-2.0) + 0.65), 1e-3);
      EXPECT_EQ(f.mode("w1"), NodeMode::kRescue);
      for (int i = 0; i < 10; ++i) {
        EXPECT_EQ(f.t.select(301 * S + 1, i / 10.0).pick->node_id, "w2");
      }
    } else {
      EXPECT_TRUE(f.rescues.empty());
      EXPECT_EQ(f.mode("w1"), NodeMode::kNormal);
    }
  }
}

TEST(NodeStateMachine, RescueBatchAndEnd) {
  NodeTable::Options o;
  o.breaker.slow_start_ms = 1;
  Fixture f(o);
  f.run({"w1", "w2"}, 0, 1 * S);
  add_exec_timeouts(f, 4, 1 * S);
  f.run({"w1", "w2"}, 302 * S, 302 * S);
  ASSERT_EQ(f.mode("w1"), NodeMode::kRescue);
  const auto before = f.t.rescue_batch("w1", false, 302 * S);
  EXPECT_EQ(before.candidates, 2);
  EXPECT_NEAR(before.timeout_count, 4, 1e-9);
  int batches = 0;
  RescueProbe r;
  do {
    r = f.t.rescue_batch("w1", true, 302 * S);
    ++batches;
  } while (!RescueTarget::reached(r.prob, 0.5) && batches < 200);
  EXPECT_LT(batches, 200);
  EXPECT_NEAR(r.timeout_count, 4 * std::pow(0.9, batches), 1e-9);
  auto c = f.t.end_rescue("w1", 302 * S, 0);
  ASSERT_TRUE(c);
  EXPECT_EQ(c->to, NodeMode::kSlowStart);
}

TEST(NodeStateMachine, LostDuringRescueEvictedAtEnd) {
  NodeTable::Options o;
  o.breaker.slow_start_ms = 1;
  Fixture f(o);
  f.run({"w1", "w2"}, 0, 1 * S);
  add_exec_timeouts(f, 4, 1 * S);
  f.run({"w1", "w2"}, 302 * S, 302 * S);
  ASSERT_EQ(f.mode("w1"), NodeMode::kRescue);
  f.run({"w2"}, 303 * S, 312 * S);  // w1 停止上报 10 s
  EXPECT_EQ(f.mode("w1"), NodeMode::kRescue);  // 急救期间不剔除
  auto c = f.t.end_rescue("w1", 312 * S, 0);
  ASSERT_TRUE(c);
  EXPECT_EQ(c->to, NodeMode::kEvicted);
  EXPECT_EQ(c->reason, "lost");
}

TEST(NodeStateMachine, ConcurrentReportsTicksProbes) {
  Fixture f;
  std::atomic<bool> stop{false};
  std::vector<std::thread> th;
  for (int k = 1; k <= 3; ++k) {
    th.emplace_back([&, k] {
      const std::string id = "w" + std::to_string(k);
      for (uint64_t i = 1; i <= 2000; ++i) {
        f.t.apply(healthy(id, i, i % 5 == 0 ? 0.99F : 0.2F), "10.0.0.1", 1000, 1000, nullptr);
        f.t.on_probe(id, i % 7 != 0, 1000, 1000);
      }
    });
  }
  th.emplace_back([&] {
    while (!stop.load()) {
      f.t.tick_states(1000, 1000, nullptr, nullptr);
      f.t.select(1000, 0.5);
      f.t.probe_targets(1000);
    }
  });
  for (int k = 0; k < 3; ++k) th[k].join();
  stop.store(true);
  th.back().join();
  EXPECT_EQ(f.t.node_ids().size(), 3U);
}

}  // namespace
