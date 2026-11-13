#include <gtest/gtest.h>

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
