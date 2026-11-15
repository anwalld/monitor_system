// MysqlWriter 集成测试：需要真实 MySQL（表结构 deploy/mysql/init.sql）。
// 设置 LM_TEST_MYSQL_HOST（以及可选 LM_TEST_MYSQL_PORT / USER / PASSWORD / DB）后运行，否则跳过。
// 用随机前缀的 node_id / task_id，测试结束删除。

#include <gtest/gtest.h>

#include <cstdlib>
#include <random>

#include "storage/mysql_writer.h"

using namespace lm::manager;

namespace {

constexpr int64_t S = 1000;
constexpr int64_t kBase = 1'800'000'000'000;  // 2027-01-15 08:00:00 UTC

std::string env(const char* k, const char* def) {
  const char* v = std::getenv(k);
  return v != nullptr && v[0] != '\0' ? v : def;
}

class MysqlWriterTest : public ::testing::Test {
 protected:
  void SetUp() override {
    if (std::getenv("LM_TEST_MYSQL_HOST") == nullptr) {
      GTEST_SKIP() << "未设置 LM_TEST_MYSQL_HOST，跳过 MySQL 集成测试";
    }
    if (!MysqlClient::compiled_in()) GTEST_SKIP() << "未编译 MySQL 支持";
    opt_.db.host = env("LM_TEST_MYSQL_HOST", "127.0.0.1");
    opt_.db.port = static_cast<unsigned>(std::stoi(env("LM_TEST_MYSQL_PORT", "3306")));
    opt_.db.user = env("LM_TEST_MYSQL_USER", "lm");
    opt_.db.password = env("LM_TEST_MYSQL_PASSWORD", "");
    opt_.db.database = env("LM_TEST_MYSQL_DB", "lm");
    p_ = "t" + std::to_string(std::random_device{}() % 100000) + "-";
    db_ = std::make_unique<MysqlClient>(opt_.db);
    ASSERT_TRUE(db_->ensure(0)) << db_->error();
  }
  void TearDown() override {
    if (!db_) return;
    for (const char* t : {"node", "node_metrics_5m", "node_event", "task"}) {
      const std::string col = std::string(t) == "task" ? "task_id" : "node_id";
      db_->exec(std::string("DELETE FROM ") + t + " WHERE " + col + " LIKE '" + p_ + "%'");
    }
  }
  std::vector<std::vector<std::string>> q(const std::string& sql) {
    auto r = db_->query(sql);
    EXPECT_TRUE(r) << db_->error() << " | " << sql;
    return r ? *r : std::vector<std::vector<std::string>>{};
  }
  MetricPoint pt(int64_t ts, double cpu) {
    MetricPoint p;
    p.node_id = p_ + "w1";
    p.ts_ms = ts;
    p.cpu = cpu;
    p.mem = 0.5;
    p.score = 0.8;
    p.rtt_ms = 2;
    p.timeout_rate = 0;
    p.retrans_rate = 0;
    return p;
  }
  TaskChange tc(const std::string& id, TaskKind k, int64_t ts, bool alloc = false) {
    TaskChange c;
    c.task_id = p_ + id;
    c.kind = k;
    c.ts_ms = ts;
    if (alloc) {
      c.node_id = p_ + "w1";
      c.input_bytes = 64;
    }
    return c;
  }

  MysqlWriter::Options opt_;
  std::string p_;
  std::unique_ptr<MysqlClient> db_;
};

TEST_F(MysqlWriterTest, FourTables) {
  MysqlQueue mq(1024);
  MysqlWriter w(mq, opt_);
  std::vector<MysqlItem> items;
  items.emplace_back(NodeRow{p_ + "w1", "10.0.0.1:50052", 4, kBase});
  for (int i = 0; i < 300; ++i) items.emplace_back(pt(kBase + i * S, i < 150 ? 0.2 : 0.4));
  items.emplace_back(pt(kBase + 300 * S, 0.9));  // 下一个桶
  NodeEvent e;
  e.node_id = p_ + "w1";
  e.kind = "evict";
  e.ts_ms = kBase + 1234;
  e.detail = R"({"from":"normal","reason":"lost"})";
  e.id = "1800000001234-0";
  items.emplace_back(e);
  items.emplace_back(tc("a", TaskKind::kAllocated, kBase, true));
  items.emplace_back(tc("a", TaskKind::kSent, kBase + 1));
  items.emplace_back(tc("v", TaskKind::kAllocated, kBase, true));
  items.emplace_back(tc("v", TaskKind::kVoid, kBase + 30 * S));
  items.emplace_back(tc("x", TaskKind::kAllocated, kBase, true));
  items.emplace_back(tc("x", TaskKind::kReceived, kBase + 2));
  w.consume(std::move(items));
  w.flush(kBase + 305 * S, 0, false);  // 第一个桶结束 + 5 秒宽限
  EXPECT_EQ(w.errors(), 0U);

  auto r = q("SELECT addr, ncpu, first_seen FROM node WHERE node_id='" + p_ + "w1'");
  ASSERT_EQ(r.size(), 1U);
  EXPECT_EQ(r[0][0], "10.0.0.1:50052");
  EXPECT_EQ(r[0][1], "4");
  EXPECT_EQ(r[0][2], "2027-01-15 08:00:00");

  r = q("SELECT bucket_start, ROUND(cpu_avg,3), ROUND(cpu_max,3), samples, runq_p99_max_us IS NULL "
        "FROM node_metrics_5m WHERE node_id='" + p_ + "w1' ORDER BY bucket_start");
  ASSERT_EQ(r.size(), 1U);  // 第二个桶未结束
  EXPECT_EQ(r[0][0], "2027-01-15 08:00:00");
  EXPECT_EQ(r[0][1], "0.3");
  EXPECT_EQ(r[0][2], "0.4");
  EXPECT_EQ(r[0][3], "300");
  EXPECT_EQ(r[0][4], "1");

  r = q("SELECT kind, ts, JSON_UNQUOTE(JSON_EXTRACT(detail,'$.event_id')), "
        "JSON_UNQUOTE(JSON_EXTRACT(detail,'$.reason')) FROM node_event WHERE node_id='" + p_ + "w1'");
  ASSERT_EQ(r.size(), 1U);
  EXPECT_EQ(r[0][0], "evict");
  EXPECT_EQ(r[0][1], "2027-01-15 08:00:01.234");
  EXPECT_EQ(r[0][2], "1800000001234-0");
  EXPECT_EQ(r[0][3], "lost");

  // 下一批：x 执行超时后迟到完成；a 收到并完成；v 作废后迟到的 done 不覆盖
  w.consume({tc("x", TaskKind::kTimeout, kBase + 300 * S)});
  w.flush(kBase + 306 * S, 0, false);
  w.consume({tc("x", TaskKind::kDone, kBase + 301 * S), tc("a", TaskKind::kReceived, kBase + 2),
             tc("a", TaskKind::kDone, kBase + 3), tc("v", TaskKind::kDone, kBase + 40 * S)});
  w.flush(kBase + 307 * S, 0, false);
  EXPECT_EQ(w.errors(), 0U);
  r = q("SELECT task_id, state, node_id, input_bytes, t_alloc, t_sent IS NULL, t_recv IS NULL, "
        "t_done FROM task WHERE task_id LIKE '" + p_ + "%' ORDER BY task_id");
  ASSERT_EQ(r.size(), 3U);
  EXPECT_EQ(r[0][1], "done");  // a
  EXPECT_EQ(r[0][2], p_ + "w1");
  EXPECT_EQ(r[0][3], "64");
  EXPECT_EQ(r[0][4], "2027-01-15 08:00:00.000");
  EXPECT_EQ(r[0][5], "0");
  EXPECT_EQ(r[0][6], "0");
  EXPECT_EQ(r[0][7], "2027-01-15 08:00:00.003");
  EXPECT_EQ(r[1][1], "void");  // v
  EXPECT_EQ(r[2][1], "done");  // x：timeout → done
  EXPECT_EQ(r[2][7], "2027-01-15 08:05:01.000");

  // 退出：未结束的桶也写出；同一桶再写一次覆盖而不重复
  w.flush(kBase + 308 * S, 0, true);
  r = q("SELECT samples FROM node_metrics_5m WHERE node_id='" + p_ + "w1' ORDER BY bucket_start");
  ASSERT_EQ(r.size(), 2U);
  EXPECT_EQ(r[1][0], "1");
  w.consume({pt(kBase + 301 * S, 0.5), pt(kBase + 302 * S, 0.5)});
  w.flush(kBase + 309 * S, 0, true);
  r = q("SELECT samples FROM node_metrics_5m WHERE node_id='" + p_ + "w1' ORDER BY bucket_start");
  ASSERT_EQ(r.size(), 2U);
  EXPECT_EQ(r[1][0], "2");
}

TEST_F(MysqlWriterTest, LostConnectionKeepsBatchForRetry) {
  MysqlQueue mq(64);
  MysqlWriter w(mq, opt_);
  w.consume({NodeRow{p_ + "w1", "10.0.0.1:50052", 4, kBase}});
  w.flush(kBase, 0, false);  // 建立连接
  ASSERT_EQ(w.errors(), 0U);
  // 掐掉写线程的连接（模拟 MySQL 重启 / 网络中断）
  auto ids = q("SELECT id FROM information_schema.processlist WHERE user='" + opt_.db.user +
               "' AND id <> CONNECTION_ID()");
  for (const auto& r : ids) db_->exec("KILL " + r[0]);
  w.consume({tc("k", TaskKind::kAllocated, kBase, true), tc("k", TaskKind::kSent, kBase + 1),
             pt(kBase + 1000, 0.5)});
  w.flush(kBase + 306 * S, 1, false);  // 连接已断：失败，这一批留在缓冲
  EXPECT_GE(w.errors(), 1U);
  EXPECT_TRUE(q("SELECT 1 FROM task WHERE task_id='" + p_ + "k'").empty());
  w.consume({tc("k", TaskKind::kReceived, kBase + 2)});
  w.flush(kBase + 307 * S, 10'000, false);  // 超过重连间隔：重连后写入
  auto r = q("SELECT state, t_sent IS NULL FROM task WHERE task_id='" + p_ + "k'");
  ASSERT_EQ(r.size(), 1U);
  EXPECT_EQ(r[0][0], "received");
  EXPECT_EQ(r[0][1], "0");
  EXPECT_EQ(q("SELECT samples FROM node_metrics_5m WHERE node_id='" + p_ + "w1'").size(), 1U);
}

TEST_F(MysqlWriterTest, ThreadWritesAndUnreachableDoesNotBlock) {
  MysqlQueue mq(1024);
  auto o = opt_;
  o.flush_interval = std::chrono::milliseconds(100);
  MysqlWriter w(mq, o);
  w.start();
  NodeEvent e;
  e.node_id = p_ + "w2";
  e.kind = "normal";
  e.ts_ms = kBase;
  e.detail = "{}";
  mq.push(e);
  for (int i = 0; i < 50 && w.rows_written() == 0; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  EXPECT_EQ(q("SELECT kind FROM node_event WHERE node_id='" + p_ + "w2'").size(), 1U);
  w.stop();

  // 连不上：缓冲留在内存，stop 不卡住
  auto bad = opt_;
  bad.db.port = 1;
  bad.flush_interval = std::chrono::milliseconds(50);
  MysqlQueue mq2(16);
  MysqlWriter w2(mq2, bad);
  w2.start();
  mq2.push(e);
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  const auto t0 = std::chrono::steady_clock::now();
  w2.stop();
  EXPECT_LT(std::chrono::steady_clock::now() - t0, std::chrono::seconds(3));
  EXPECT_EQ(w2.rows_written(), 0U);
}

}  // namespace
