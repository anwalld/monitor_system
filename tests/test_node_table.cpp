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
