// RedisWriter 集成测试：需要真实 Redis。
// 设置 LM_TEST_REDIS_URI（如 tcp://127.0.0.1:6379）与可选 LM_TEST_REDIS_PASSWORD 后运行，否则跳过。
// 所有 key 带随机前缀，测试结束删除。

#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>
#include <random>

#include <sw/redis++/redis++.h>

#include "storage/redis_writer.h"

using namespace lm::manager;

namespace {

class RedisWriterTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const char* uri = std::getenv("LM_TEST_REDIS_URI");
    if (uri == nullptr) GTEST_SKIP() << "未设置 LM_TEST_REDIS_URI，跳过 Redis 集成测试";
    opt_.uri = uri;
    if (const char* pw = std::getenv("LM_TEST_REDIS_PASSWORD")) opt_.password = pw;
    opt_.key_prefix = "lmtest:" + std::to_string(std::random_device{}()) + ":";
    sw::redis::ConnectionOptions co;
    RedisWriter::parse_uri(opt_.uri, co);
    if (!opt_.password.empty()) co.password = opt_.password;
    redis_ = std::make_unique<sw::redis::Redis>(co);
  }
  void TearDown() override {
    if (!redis_) return;
    std::vector<std::string> keys;
    redis_->scan(0, opt_.key_prefix + "*", 1000, std::back_inserter(keys));
    if (!keys.empty()) redis_->del(keys.begin(), keys.end());
  }
  AcceptedSample acc(const std::string& id, uint64_t seq, int64_t ts) {
    AcceptedSample a;
    a.addr = "10.0.0.1:50052";
    a.ncpu = 2;
    a.recv_wall_ms = ts + 5;
    a.sample.set_node_id(id);
    a.sample.set_seq(seq);
    a.sample.set_ts_ms(ts);
    a.sample.set_cpu_busy(0.25F);
    a.sample.add_core_busy(0.2F);
    a.sample.add_core_busy(0.3F);
    return a;
  }
  std::string k(const std::string& s) const { return opt_.key_prefix + s; }

  RedisWriter::Options opt_;
  std::unique_ptr<sw::redis::Redis> redis_;
};

TEST_F(RedisWriterTest, WritesHashTimeSeriesAndStream) {
  StorageQueue q(16);
  RedisWriter w(q, opt_);
  const int64_t now = 1'800'000'000'000;
  w.write_batch({acc("w1", 1, now - 1000), acc("w2", 1, now - 1000), acc("w1", 2, now)});
  w.flush_stream(now);

  std::unordered_map<std::string, std::string> h;
  redis_->hgetall(k("node:w1"), std::inserter(h, h.begin()));
  EXPECT_EQ(h["seq"], "2");
  EXPECT_EQ(h["addr"], "10.0.0.1:50052");
  EXPECT_EQ(h["ncpu"], "2");
  EXPECT_EQ(h["cpu_busy"], "0.2500");
  for (const char* f : {"ts_ms", "mem_avail_ratio", "disk_util", "last_seen_ms"}) {
    EXPECT_TRUE(h.count(f)) << f;
  }

  EXPECT_EQ(redis_->zcount(k("node:w1:ts"), sw::redis::UnboundedInterval<double>{}), 2);

  using Attrs = std::vector<std::pair<std::string, std::string>>;
  std::vector<std::pair<std::string, Attrs>> items;
  redis_->xrevrange(k("stream:metrics"), "+", "-", 1, std::back_inserter(items));
  ASSERT_EQ(items.size(), 1U);
  std::string nodes;
  for (auto& [f, v] : items[0].second) {
    if (f == "nodes") nodes = v;
  }
  EXPECT_NE(nodes.find("\"node_id\":\"w1\""), std::string::npos);
  EXPECT_NE(nodes.find("\"node_id\":\"w2\""), std::string::npos);
  EXPECT_NE(nodes.find("\"seq\":\"2\""), std::string::npos);  // w1 只保留这一秒最新的一条
  EXPECT_EQ(w.write_errors(), 0U);
}

TEST_F(RedisWriterTest, RunqFieldsWrittenZeroKeptAndClearedWhenAbsent) {
  StorageQueue q(16);
  RedisWriter w(q, opt_);
  const int64_t now = 1'800'000'000'000;
  auto with = acc("w1", 1, now - 1000);
  with.sample.set_runq_p50_us(12.5F);
  with.sample.set_runq_p99_us(850.0F);
  with.sample.set_runq_drop_rate(0.0F);
  w.write_batch({with});

  std::unordered_map<std::string, std::string> h;
  redis_->hgetall(k("node:w1"), std::inserter(h, h.begin()));
  EXPECT_EQ(h["runq_p99_us"], "850.0000");
  EXPECT_EQ(h["runq_p50_us"], "12.5000");
  ASSERT_TRUE(h.count("runq_drop_rate"));  // 值为 0 也写
  EXPECT_EQ(h["runq_drop_rate"], "0.0000");
  std::vector<std::string> members;
  redis_->zrange(k("node:w1:ts"), 0, -1, std::back_inserter(members));
  ASSERT_EQ(members.size(), 1U);
  EXPECT_NE(members[0].find("\"runq_p99_us\":850"), std::string::npos) << members[0];
  EXPECT_NE(members[0].find("\"runq_drop_rate\":0"), std::string::npos) << members[0];

  // 之后的样本不带调度延迟（如 Worker 以 runq=off 重启）：字段被删除，JSON 不含
  w.write_batch({acc("w1", 2, now)});
  EXPECT_FALSE(redis_->hexists(k("node:w1"), "runq_p99_us"));
  EXPECT_FALSE(redis_->hexists(k("node:w1"), "runq_drop_rate"));
  members.clear();
  redis_->zrangebyscore(k("node:w1:ts"), sw::redis::BoundedInterval<double>(
                                             static_cast<double>(now), static_cast<double>(now),
                                             sw::redis::BoundType::CLOSED),
                        std::back_inserter(members));
  ASSERT_EQ(members.size(), 1U);
  EXPECT_EQ(members[0].find("runq"), std::string::npos) << members[0];
  EXPECT_EQ(w.write_errors(), 0U);
}

TEST_F(RedisWriterTest, ScoreAndEvictionFields) {
  StorageQueue q(16);
  RedisWriter w(q, opt_);
  const int64_t now = 1'800'000'000'000;
  auto a = acc("w1", 1, now);
  a.score.total = 1.0;
  a.score.cpu = 1.0;
  a.score.runq = 0.3;
  a.score.mem = 0.75;
  a.score.net = 0.5;
  a.score.best_nic = "ens160";
  w.write_batch({a});
  std::unordered_map<std::string, std::string> h;
  redis_->hgetall(k("node:w1"), std::inserter(h, h.begin()));
  EXPECT_EQ(h["score"], "1.0000");
  EXPECT_EQ(h["s_cpu"], "1.0000");
  EXPECT_EQ(h["s_runq"], "0.3000");
  EXPECT_EQ(h["s_mem"], "0.7500");
  EXPECT_EQ(h["s_net"], "0.5000");
  EXPECT_EQ(h["best_nic"], "ens160");
  EXPECT_EQ(h["evicted"], "0");
  ASSERT_TRUE(h.count("evict_reason"));
  EXPECT_EQ(h["evict_reason"], "");
  EXPECT_EQ(h["inflight_tasks"], "0");
  EXPECT_EQ(h["f_timeout"], "1.0000");

  auto b = acc("w1", 2, now + 1000);
  b.evicted = true;
  b.evict_reason = "cpu";
  b.inflight.inflight_tasks = 3;
  b.inflight.inflight_bytes = 3072;
  b.inflight.unconfirmed = 2;
  b.inflight.f_inflight = 0.5;
  b.inflight.f_timeout = std::exp(-0.5);
  b.inflight.timeout_count = 1;
  w.write_batch({b});
  h.clear();
  redis_->hgetall(k("node:w1"), std::inserter(h, h.begin()));
  EXPECT_EQ(h["evicted"], "1");
  EXPECT_EQ(h["evict_reason"], "cpu");
  EXPECT_EQ(h["best_nic"], "");
  EXPECT_EQ(h["inflight_tasks"], "3");
  EXPECT_EQ(h["inflight_bytes"], "3072");
  EXPECT_EQ(h["unconfirmed"], "2");
  EXPECT_EQ(h["f_inflight"], "0.5000");
  EXPECT_EQ(h["f_timeout"], "0.6065");
  EXPECT_EQ(h["timeout_count"], "1.0000");
  EXPECT_EQ(w.write_errors(), 0U);
}

TEST_F(RedisWriterTest, StateFieldsAndStateOnlyWrite) {
  StorageQueue q(16);
  RedisWriter w(q, opt_);
  const int64_t now = 1'800'000'000'000;
  auto a = acc("w1", 1, now);
  a.mode = NodeMode::kSlowStart;
  a.mode_since_wall_ms = now - 10;
  a.f_slow = 0.2;
  w.write_batch({a});
  std::unordered_map<std::string, std::string> h;
  redis_->hgetall(k("node:w1"), std::inserter(h, h.begin()));
  EXPECT_EQ(h["state"], "slow_start");
  EXPECT_EQ(h["state_reason"], "");
  EXPECT_EQ(h["state_since_ms"], std::to_string(now - 10));
  EXPECT_EQ(h["f_slow"], "0.2000");
  EXPECT_EQ(h["probe_fail_streak"], "0");

  AcceptedSample st;
  st.state_only = true;
  st.node_id = "w1";
  st.mode = NodeMode::kEvicted;
  st.mode_reason = "lost";
  st.mode_since_wall_ms = now + 3000;
  st.f_slow = 1;
  w.write_batch({st});
  h.clear();
  redis_->hgetall(k("node:w1"), std::inserter(h, h.begin()));
  EXPECT_EQ(h["state"], "evicted");
  EXPECT_EQ(h["state_reason"], "lost");
  EXPECT_EQ(h["seq"], "1");  // 其他字段不动
  EXPECT_EQ(redis_->zcount(k("node:w1:ts"), sw::redis::UnboundedInterval<double>{}), 1);
  EXPECT_EQ(w.write_errors(), 0U);
}

TEST_F(RedisWriterTest, TrimRemovesOlderThanRetention) {
  StorageQueue q(16);
  opt_.ts_retention_ms = 3600 * 1000;
  RedisWriter w(q, opt_);
  const int64_t now = 1'800'000'000'000;
  w.write_batch({acc("w1", 1, now - 3600 * 1000 - 1), acc("w1", 2, now - 3599 * 1000), acc("w1", 3, now)});
  w.trim(now);
  EXPECT_EQ(redis_->zcount(k("node:w1:ts"), sw::redis::UnboundedInterval<double>{}), 2);
}

TEST(RedisUri, Parses) {
  sw::redis::ConnectionOptions co;
  RedisWriter::parse_uri("tcp://10.0.0.5:6380", co);
  EXPECT_EQ(co.host, "10.0.0.5");
  EXPECT_EQ(co.port, 6380);
  RedisWriter::parse_uri("redis-host", co);
  EXPECT_EQ(co.host, "redis-host");
  EXPECT_EQ(co.port, 6379);
  EXPECT_THROW(RedisWriter::parse_uri("unix:///tmp/r.sock", co), std::invalid_argument);
  EXPECT_THROW(RedisWriter::parse_uri("tcp://h:abc", co), std::invalid_argument);
}

TEST(RedisWriterOffline, FailuresAreCountedNotThrown) {
  StorageQueue q(16);
  RedisWriter::Options o;
  o.uri = "tcp://127.0.0.1:1";  // 无人监听
  RedisWriter w(q, o);
  AcceptedSample a;
  a.sample.set_node_id("w1");
  a.sample.set_seq(1);
  EXPECT_NO_THROW(w.write_batch({a}));
  EXPECT_NO_THROW(w.flush_stream(0));
  EXPECT_GE(w.write_errors(), 1U);
}

}  // namespace
