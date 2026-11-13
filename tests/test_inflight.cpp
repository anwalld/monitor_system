// 在途任务账（模拟时钟）与 NodeTable 上的在途因子。

#include <gtest/gtest.h>

#include <atomic>
#include <cmath>
#include <random>
#include <thread>

#include "inflight/inflight_ledger.h"
#include "ingest/node_table.h"
#include "lm/clock.h"
#include "scheduler_loop.h"

using namespace lm::manager;
using Kind = InflightLedger::Expired::Kind;
using State = InflightLedger::State;

namespace {

constexpr int64_t S = 1000;  // 1 秒

TEST(InflightParams, DefaultsAndInvalid) {
  const auto p = InflightParams::from_config(lm::Config::load_string("{}"));
  EXPECT_EQ(p.ack_timeout_ms, 30 * S);
  EXPECT_EQ(p.exec_timeout_ms, 300 * S);
  EXPECT_DOUBLE_EQ(p.lambda, 0.5);
  EXPECT_DOUBLE_EQ(p.base_bytes_floor, 1048576);
  const auto q = InflightParams::from_config(lm::Config::load_string("inflight: {exec_timeout_s: 20}"));
  EXPECT_EQ(q.exec_timeout_ms, 20 * S);
  for (const char* y : {"inflight: {ack_timeout_s: 0}", "inflight: {lambda: -1}",
                        "inflight: {base_tasks_floor: 0}"}) {
    EXPECT_THROW(InflightParams::from_config(lm::Config::load_string(y)), lm::ConfigError) << y;
  }
}

TEST(InflightParams, ManagerYamlParses) {
  const auto p = InflightParams::from_config(lm::Config::load_file(LM_CONFIG_DIR "/manager.yaml"));
  EXPECT_EQ(p.ack_timeout_ms, 30 * S);
}

TEST(Inflight, AllocateCounts) {
  InflightLedger l;
  ASSERT_TRUE(l.add("t1", "w1", 4096, 0));
  EXPECT_FALSE(l.add("t1", "w1", 4096, 0));
  const auto* n = l.node("w1");
  EXPECT_EQ(n->inflight_tasks, 1);
  EXPECT_EQ(n->inflight_bytes, 4096);
  EXPECT_EQ(n->total, 1);
  EXPECT_EQ(*l.state("t1"), State::kAllocated);
  EXPECT_EQ(*l.deadline("t1"), 30 * S);
}

TEST(Inflight, MarkSentRestartsDeadlineAndIsIdempotent) {
  InflightLedger l;
  l.add("t1", "w1", 0, 0);
  EXPECT_EQ(l.mark_sent("t1", 20 * S), InflightLedger::MarkResult::kOk);
  EXPECT_EQ(*l.state("t1"), State::kSent);
  EXPECT_EQ(*l.deadline("t1"), 50 * S);
  EXPECT_EQ(l.mark_sent("t1", 25 * S), InflightLedger::MarkResult::kOk);
  EXPECT_EQ(*l.deadline("t1"), 50 * S);  // 重复调用不改
  EXPECT_EQ(l.mark_sent("nope", 0), InflightLedger::MarkResult::kNotFound);
  l.on_event("t1", lm::TaskEvent::RECEIVED, 26 * S);
  EXPECT_EQ(l.mark_sent("t1", 27 * S), InflightLedger::MarkResult::kOk);
  EXPECT_EQ(*l.state("t1"), State::kRunning);
}

TEST(Inflight, MarkSentMovesToTailKeepingOrder) {
  InflightLedger l;
  l.add("t1", "w1", 0, 0);
  l.add("t2", "w1", 0, 10 * S);
  l.mark_sent("t1", 20 * S);  // t1 截止 50 s，排到 t2（40 s）后面
  std::vector<InflightLedger::Expired> out;
  l.tick(40 * S, &out);
  ASSERT_EQ(out.size(), 1U);
  EXPECT_EQ(out[0].task_id, "t2");
  out.clear();
  l.tick(50 * S, &out);
  ASSERT_EQ(out.size(), 1U);
  EXPECT_EQ(out[0].task_id, "t1");
}

TEST(Inflight, NormalCompletion) {
  InflightLedger l;
  l.add("t1", "w1", 100, 0);
  l.mark_sent("t1", 1 * S);
  EXPECT_EQ(l.on_event("t1", lm::TaskEvent::RECEIVED, 2 * S), InflightLedger::EventResult::kApplied);
  EXPECT_EQ(*l.state("t1"), State::kRunning);
  EXPECT_EQ(*l.deadline("t1"), 302 * S);
  EXPECT_EQ(l.on_event("t1", lm::TaskEvent::RECEIVED, 3 * S), InflightLedger::EventResult::kIgnored);
  EXPECT_EQ(l.on_event("t1", lm::TaskEvent::DONE, 4 * S), InflightLedger::EventResult::kApplied);
  const auto* n = l.node("w1");
  EXPECT_EQ(n->inflight_tasks, 0);
  EXPECT_EQ(n->inflight_bytes, 0);
  EXPECT_EQ(n->unconfirmed, 0U);
  EXPECT_EQ(n->timeout_count, 0);
  EXPECT_EQ(l.totals().inflight, 0U);
  std::vector<InflightLedger::Expired> out;
  l.tick(1000 * S, &out);
  EXPECT_TRUE(out.empty());
}

TEST(Inflight, FailedWithoutReceived) {
  InflightLedger l;
  l.add("t1", "w1", 100, 0);
  EXPECT_EQ(l.on_event("t1", lm::TaskEvent::FAILED, 1 * S), InflightLedger::EventResult::kApplied);
  EXPECT_EQ(l.node("w1")->inflight_tasks, 0);
  EXPECT_FALSE(l.has_task("t1"));
}

TEST(Inflight, SentButUnconfirmed) {
  InflightLedger l;
  l.add("t1", "w1", 100, 0);
  l.mark_sent("t1", 1 * S);
  std::vector<InflightLedger::Expired> out;
  l.tick(31 * S - 1, &out);
  EXPECT_TRUE(out.empty());
  l.tick(31 * S, &out);
  ASSERT_EQ(out.size(), 1U);
  EXPECT_EQ(out[0].kind, Kind::kUnconfirmed);
  const auto* n = l.node("w1");
  EXPECT_EQ(n->unconfirmed, 1U);
  EXPECT_EQ(n->total, 0);
  EXPECT_EQ(n->inflight_tasks, 0);
  EXPECT_EQ(n->inflight_bytes, 0);
  EXPECT_EQ(n->timeout_count, 0);
}

TEST(Inflight, AllocatedNeverSentIsVoided) {
  InflightLedger l;
  l.add("t1", "w1", 0, 0);
  std::vector<InflightLedger::Expired> out;
  l.tick(30 * S, &out);
  ASSERT_EQ(out.size(), 1U);
  EXPECT_EQ(out[0].kind, Kind::kVoided);
  const auto* n = l.node("w1");
  EXPECT_EQ(n->unconfirmed, 0U);
  EXPECT_EQ(n->voided, 1U);
  EXPECT_EQ(n->timeout_count, 0);
  EXPECT_EQ(n->total, 0);
}

TEST(Inflight, ExecTimeoutThenLateDone) {
  InflightLedger l;
  l.add("t1", "w1", 0, 0);
  l.on_event("t1", lm::TaskEvent::RECEIVED, 10 * S);
  std::vector<InflightLedger::Expired> out;
  l.tick(310 * S - 1, &out);
  EXPECT_TRUE(out.empty());
  l.tick(310 * S, &out);
  ASSERT_EQ(out.size(), 1U);
  EXPECT_EQ(out[0].kind, Kind::kExecTimeout);
  const auto* n = l.node("w1");
  EXPECT_EQ(n->timeout_count, 1);
  EXPECT_EQ(n->inflight_tasks, 0);
  EXPECT_EQ(n->total, 1);  // 执行超时仍算在总数里
  EXPECT_EQ(l.on_event("t1", lm::TaskEvent::DONE, 400 * S), InflightLedger::EventResult::kLate);
  EXPECT_EQ(n->timeout_count, 1);
  EXPECT_EQ(n->inflight_tasks, 0);
  EXPECT_EQ(l.totals().late_events, 1U);
}

TEST(Inflight, InflightFactor) {
  InflightLedger l;
  for (int i = 0; i < 6; ++i) l.add("t" + std::to_string(i), "w1", 0, 0);
  EXPECT_NEAR(l.factors("w1").inflight, 1.0 / 3, 1e-9);
  InflightLedger m;
  m.add("a", "w2", 0, 0);
  m.add("b", "w2", 0, 0);
  EXPECT_DOUBLE_EQ(m.factors("w2").inflight, 1);
  // 字节数：3 MiB / 1 MiB 下限 → r = 3
  InflightLedger b;
  b.add("x", "w3", 3 << 20, 0);
  EXPECT_NEAR(b.factors("w3").inflight, 1.0 / 3, 1e-9);
  EXPECT_DOUBLE_EQ(l.factors("unknown").inflight, 1);
}

TEST(Inflight, BaselineAdaptsSoSteadyLoadIsNotPenalized) {
  InflightLedger l;
  for (int i = 0; i < 6; ++i) l.add("t" + std::to_string(i), "w1", 0, 0);
  for (int s = 0; s < 600; ++s) l.tick_second(1.0);  // 10 个时间常数
  EXPECT_NEAR(l.node("w1")->base_tasks, 6, 1e-3);
  EXPECT_NEAR(l.factors("w1").inflight, 1, 1e-3);
  // 一秒后的单步更新量
  InflightLedger k;
  k.add("t", "w1", 0, 0);
  k.tick_second(1.0);
  EXPECT_NEAR(k.node("w1")->base_tasks, 1 - std::exp(-1.0 / 60), 1e-12);
}

TEST(Inflight, TimeoutFactorAndDecay) {
  InflightLedger l;
  for (int i = 0; i < 2; ++i) {
    const std::string id = "t" + std::to_string(i);
    l.add(id, "w1", 0, 0);
    l.on_event(id, lm::TaskEvent::RECEIVED, 0);
  }
  l.tick(300 * S, nullptr);
  EXPECT_NEAR(l.factors("w1").timeout, std::exp(-1.0), 1e-9);
  for (int s = 0; s < 600; ++s) l.tick_second(1.0);
  EXPECT_NEAR(l.node("w1")->timeout_count, 1, 1e-9);
  EXPECT_NEAR(l.factors("w1").timeout, 0.607, 1e-3);
}

// ---------- NodeTable 上的在途因子 ----------

lm::Sample healthy(const std::string& id, uint64_t seq) {
  lm::Sample s;
  s.set_node_id(id);
  s.set_seq(seq);
  s.add_core_busy(0.1F);
  s.set_cpu_busy(0.1F);
  s.set_mem_avail_ratio(0.8F);
  s.set_runq_p99_us(50.0F);
  auto* n = s.add_nics();
  n->set_name("eth0");
  n->set_rtt_ms(1);
  return s;
}

TEST(NodeTableInflight, TimeoutsReduceSelectionProbability) {
  NodeTable t({});
  t.apply(healthy("w1", 1), "10.0.0.1", 0, 0, nullptr);
  t.apply(healthy("w2", 1), "10.0.0.2", 0, 0, nullptr);
  // 让 w1 产生 2 次执行超时：u = 0 总是选中排序第一的 w1
  for (int i = 0; i < 2; ++i) {
    const std::string id = "x" + std::to_string(i);
    auto r = t.allocate(0, 0.0, id, 0);
    ASSERT_EQ(r.pick->node_id, "w1");
    t.on_event(id, lm::TaskEvent::RECEIVED, 0);
  }
  t.tick(300 * S, nullptr);
  t.apply(healthy("w1", 2), "10.0.0.1", 300 * S, 0, nullptr);
  t.apply(healthy("w2", 2), "10.0.0.2", 300 * S, 0, nullptr);
  const double want = std::exp(-1.0) / (std::exp(-1.0) + 1);
  EXPECT_NEAR(t.select(300 * S, 0.0).pick->prob, want, 1e-9);
  std::mt19937_64 rng(3);
  std::uniform_real_distribution<double> uni(0, 1);
  int w1 = 0;
  const int n = 100000;
  for (int i = 0; i < n; ++i) {
    if (t.select(300 * S, uni(rng)).pick->node_id == "w1") ++w1;
  }
  EXPECT_NEAR(static_cast<double>(w1) / n, want, 0.01);
}

TEST(NodeTableInflight, PileUpReducesWeightAndSnapshotCarried) {
  NodeTable::Options o;
  o.breaker.slow_start_ms = 1;  // 让节点立即离开慢启动（慢启动中 f_inflight 固定为 1）
  NodeTable t(o);
  t.apply(healthy("w1", 1), "10.0.0.1", 0, 0, nullptr);
  t.tick_states(1, 1, nullptr, nullptr);
  ASSERT_EQ(*t.mode("w1"), NodeMode::kNormal);
  for (int i = 0; i < 6; ++i) t.allocate(1, 0.5, "t" + std::to_string(i), 0);
  EXPECT_NEAR(t.select(1, 0.5).pick->weight, 1.0 / 3, 1e-9);
  AcceptedSample a;
  t.apply(healthy("w1", 2), "10.0.0.1", 10, 0, &a);
  EXPECT_EQ(a.inflight.inflight_tasks, 6);
  EXPECT_NEAR(a.inflight.f_inflight, 1.0 / 3, 1e-9);
  EXPECT_DOUBLE_EQ(a.inflight.f_timeout, 1);
}

TEST(NodeTableInflight, ConcurrentAllocateEventsAndTicks) {
  NodeTable t({});
  t.apply(healthy("w1", 1), "10.0.0.1", 0, 0, nullptr);
  t.apply(healthy("w2", 1), "10.0.0.2", 0, 0, nullptr);
  std::atomic<bool> stop{false};
  std::vector<std::thread> th;
  for (int k = 0; k < 3; ++k) {
    th.emplace_back([&, k] {
      for (int i = 0; i < 2000; ++i) {
        const std::string id = std::to_string(k) + "-" + std::to_string(i);
        t.allocate(0, (i % 10) / 10.0, id, 10);
        t.mark_sent(id, 0);
        t.on_event(id, lm::TaskEvent::RECEIVED, 0);
        t.on_event(id, lm::TaskEvent::DONE, 0);
      }
    });
  }
  th.emplace_back([&] {
    while (!stop.load()) {
      t.tick(0, nullptr);
      t.tick_second(0.1);
    }
  });
  for (int k = 0; k < 3; ++k) th[k].join();
  stop.store(true);
  th.back().join();
  EXPECT_EQ(t.inflight_totals().inflight, 0U);
  EXPECT_EQ(t.inflight_snapshot("w1").inflight_tasks, 0);
  EXPECT_EQ(t.inflight_snapshot("w2").inflight_bytes, 0);
}

TEST(SchedulerLoop, ProcessesTimeoutsAndCounts) {
  NodeTable::Options o;
  o.inflight.ack_timeout_ms = 100;
  NodeTable t(o);
  t.apply(healthy("w1", 1), "10.0.0.1", 0, 0, nullptr);
  t.allocate(0, 0.5, "a", 0);
  t.allocate(0, 0.5, "b", 0);
  t.mark_sent("b", 0);
  SchedulerLoop loop(t, {});
  loop.tick_once(50);
  EXPECT_EQ(t.inflight_totals().inflight, 2U);
  loop.tick_once(100);
  EXPECT_EQ(loop.voided(), 1U);
  EXPECT_EQ(loop.unconfirmed(), 1U);
  EXPECT_EQ(t.inflight_totals().inflight, 0U);
}

TEST(SchedulerLoop, ThreadRunsTicks) {
  NodeTable::Options o;
  o.inflight.ack_timeout_ms = 50;
  NodeTable t(o);
  t.apply(healthy("w1", 1), "10.0.0.1", lm::mono_ms(), 0, nullptr);
  t.allocate(lm::mono_ms(), 0.5, "a", 0);
  SchedulerLoop loop(t, {std::chrono::milliseconds(10), std::chrono::milliseconds(100)});
  loop.start();
  for (int i = 0; i < 100 && loop.voided() == 0; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  loop.stop();
  EXPECT_EQ(loop.voided(), 1U);
}

}  // namespace
