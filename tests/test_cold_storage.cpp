// M7a 冷数据纯逻辑：5 分钟聚合、任务合并、事件生成、SQL 生成、选中概率与任务变化的生产者。

#include <gtest/gtest.h>

#include <cmath>
#include <map>

#include "api/scheduler_service.h"
#include "ingest/collector_service.h"
#include "ingest/node_table.h"
#include "lm/clock.h"
#include "scheduler_loop.h"
#include "storage/events.h"
#include "storage/metrics_agg.h"
#include "storage/mysql_writer.h"
#include "storage/task_rows.h"

using namespace lm::manager;

namespace {

constexpr int64_t S = 1000;
constexpr int64_t kBase = 1'800'000'000'000;  // 2027-01-15 08:00:00 UTC，正好是 5 分钟边界

MetricPoint pt(const std::string& id, int64_t ts, double cpu, double mem, double score) {
  MetricPoint p;
  p.node_id = id;
  p.ts_ms = ts;
  p.cpu = cpu;
  p.mem = mem;
  p.score = score;
  return p;
}

// ---------- 5 分钟聚合 ----------

TEST(MetricsAggregator, BucketAlignment) {
  MetricsAggregator a;
  ASSERT_EQ(kBase % (300 * S), 0);
  EXPECT_EQ(a.bucket_of(kBase), kBase);
  EXPECT_EQ(a.bucket_of(kBase + 299'999), kBase);
  EXPECT_EQ(a.bucket_of(kBase + 300'000), kBase + 300'000);
  EXPECT_EQ(a.bucket_of(kBase - 1), kBase - 300'000);
  EXPECT_EQ(mysql_datetime(kBase, false), "2027-01-15 08:00:00");
  EXPECT_EQ(mysql_datetime(kBase + 123), "2027-01-15 08:00:00.123");
}

TEST(MetricsAggregator, SpecExampleAndMissingColumns) {
  MetricsAggregator a;
  // spec 例子：三个样本 cpu 0.2/0.4/0.9，mem 0.5/0.3/0.4，score 0.9/0.8/0.4
  auto p1 = pt("w1", kBase + 1 * S, 0.2, 0.5, 0.9);
  p1.runq_p99_us = 100;
  p1.rtt_ms = 2;
  p1.timeout_rate = 0.1;
  p1.retrans_rate = 0.0;
  auto p2 = pt("w1", kBase + 2 * S, 0.4, 0.3, 0.8);
  p2.runq_p99_us = 900;  // 网卡缺失：不计入网络平均
  auto p3 = pt("w1", kBase + 3 * S, 0.9, 0.4, 0.4);
  p3.rtt_ms = 4;
  p3.timeout_rate = 0.3;
  p3.retrans_rate = 0.02;
  a.add(p1);
  a.add(p2);
  a.add(p3);
  a.add(pt("w2", kBase + 1 * S, 0.5, 0.5, 0.5));  // 没有调度延迟、没有网卡
  auto rows = a.take_all();
  ASSERT_EQ(rows.size(), 2U);
  const auto& r = rows[0].node_id == "w1" ? rows[0] : rows[1];
  const auto& r2 = rows[0].node_id == "w1" ? rows[1] : rows[0];
  EXPECT_EQ(r.bucket_start_ms, kBase);
  EXPECT_EQ(r.samples, 3);
  EXPECT_NEAR(r.cpu_avg, 0.5, 1e-12);
  EXPECT_NEAR(r.cpu_max, 0.9, 1e-12);
  EXPECT_NEAR(r.mem_avail_min, 0.3, 1e-12);
  EXPECT_NEAR(*r.runq_p99_max_us, 900, 1e-12);
  EXPECT_NEAR(*r.net_rtt_avg_ms, 3, 1e-12);
  EXPECT_NEAR(*r.net_timeout_rate, 0.2, 1e-12);
  EXPECT_NEAR(*r.net_retrans_rate, 0.01, 1e-12);
  EXPECT_NEAR(r.score_avg, 0.7, 1e-12);
  EXPECT_FALSE(r2.runq_p99_max_us);
  EXPECT_FALSE(r2.net_rtt_avg_ms);
  EXPECT_FALSE(r2.net_timeout_rate);
  EXPECT_EQ(a.open_buckets(), 0U);
}

TEST(MetricsAggregator, SpecAggregationExample) {
  // spec「聚合口径」：CPU 0.2/0.4/0.6，P99 100/300/200 µs，可用内存 0.5/0.3/0.4
  MetricsAggregator a;
  const double cpu[] = {0.2, 0.4, 0.6}, p99[] = {100, 300, 200}, mem[] = {0.5, 0.3, 0.4};
  for (int i = 0; i < 3; ++i) {
    auto p = pt("w1", kBase + i * S, cpu[i], mem[i], 1);
    p.runq_p99_us = p99[i];
    a.add(p);
  }
  const auto rows = a.take_all();
  ASSERT_EQ(rows.size(), 1U);
  EXPECT_NEAR(rows[0].cpu_avg, 0.4, 1e-12);
  EXPECT_NEAR(rows[0].cpu_max, 0.6, 1e-12);
  EXPECT_NEAR(*rows[0].runq_p99_max_us, 300, 1e-12);
  EXPECT_NEAR(rows[0].mem_avail_min, 0.3, 1e-12);
  EXPECT_EQ(rows[0].samples, 3);
}

TEST(MetricsAggregator, GracePeriodAndSeparateBuckets) {
  MetricsAggregator a(300 * S, 5 * S);
  a.add(pt("w1", kBase + 299 * S, 0.1, 0.5, 1));
  a.add(pt("w1", kBase + 300 * S, 0.2, 0.5, 1));  // 下一个桶
  EXPECT_TRUE(a.take_finished(kBase + 300 * S).empty());
  EXPECT_TRUE(a.take_finished(kBase + 304'999).empty());
  a.add(pt("w1", kBase + 299'500, 0.3, 0.5, 1));  // 迟到 4.5 秒的样本仍进第一个桶
  auto rows = a.take_finished(kBase + 305 * S);
  ASSERT_EQ(rows.size(), 1U);
  EXPECT_EQ(rows[0].bucket_start_ms, kBase);
  EXPECT_EQ(rows[0].samples, 2);
  EXPECT_EQ(a.open_buckets(), 1U);
  rows = a.take_all();  // 退出时：未结束的桶也写
  ASSERT_EQ(rows.size(), 1U);
  EXPECT_EQ(rows[0].bucket_start_ms, kBase + 300 * S);
  EXPECT_EQ(rows[0].samples, 1);
}

// ---------- 任务合并 ----------

TaskChange tc(const std::string& id, TaskKind k, int64_t ts, const std::string& node = "",
              uint64_t bytes = 0) {
  TaskChange c;
  c.task_id = id;
  c.kind = k;
  c.ts_ms = ts;
  c.node_id = node;
  c.input_bytes = bytes;
  return c;
}

TEST(TaskMerger, NormalTaskInOneBatch) {
  TaskMerger m;
  m.add(tc("t1", TaskKind::kAllocated, 1, "w1", 100));
  m.add(tc("t1", TaskKind::kSent, 2));
  m.add(tc("t1", TaskKind::kReceived, 3));
  m.add(tc("t1", TaskKind::kDone, 4));
  auto rows = m.take();
  ASSERT_EQ(rows.size(), 1U);
  const auto& r = rows[0];
  EXPECT_TRUE(r.has_alloc);
  EXPECT_EQ(r.node_id, "w1");
  EXPECT_EQ(r.input_bytes, 100U);
  EXPECT_EQ(r.state, TaskKind::kDone);
  EXPECT_EQ(*r.t_alloc, 1);
  EXPECT_EQ(*r.t_sent, 2);
  EXPECT_EQ(*r.t_recv, 3);
  EXPECT_EQ(*r.t_done, 4);
  EXPECT_EQ(m.size(), 0U);
}

TEST(TaskMerger, OutOfOrderAndLateCompletion) {
  TaskMerger m;
  // 乱序：received 先于 sent 到达，状态不回退，时间各自保留
  m.add(tc("t1", TaskKind::kAllocated, 1, "w1"));
  m.add(tc("t1", TaskKind::kReceived, 3));
  m.add(tc("t1", TaskKind::kSent, 2));
  auto rows = m.take();
  ASSERT_EQ(rows.size(), 1U);
  EXPECT_EQ(rows[0].state, TaskKind::kReceived);
  EXPECT_EQ(*rows[0].t_sent, 2);
  // 下一批：超时后迟到的完成（无分配信息 → UPDATE）
  m.add(tc("t1", TaskKind::kTimeout, 300));
  m.add(tc("t1", TaskKind::kDone, 301));
  rows = m.take();
  ASSERT_EQ(rows.size(), 1U);
  EXPECT_FALSE(rows[0].has_alloc);
  EXPECT_EQ(rows[0].state, TaskKind::kDone);
  EXPECT_EQ(*rows[0].t_done, 301);
  // 终态之间：done 之后的 timeout 不覆盖
  m.add(tc("t2", TaskKind::kDone, 5));
  m.add(tc("t2", TaskKind::kTimeout, 6));
  EXPECT_EQ(m.take()[0].state, TaskKind::kDone);
}

TEST(TaskMerger, VoidAndCap) {
  TaskMerger m(2);
  m.add(tc("t1", TaskKind::kAllocated, 1, "w1"));
  m.add(tc("t1", TaskKind::kVoid, 30001));
  m.add(tc("t2", TaskKind::kAllocated, 1, "w1"));
  m.add(tc("t3", TaskKind::kAllocated, 1, "w1"));  // 超过上限
  EXPECT_EQ(m.dropped(), 1U);
  auto rows = m.take();
  ASSERT_EQ(rows.size(), 2U);
  EXPECT_EQ(rows[0].state, TaskKind::kVoid);
  EXPECT_FALSE(rows[0].t_done);
}

TEST(TaskMerger, RestoreAfterLostConnection) {
  TaskMerger m;
  m.add(tc("t1", TaskKind::kAllocated, 1, "w1", 7));
  m.add(tc("t2", TaskKind::kAllocated, 1, "w1"));
  m.add(tc("t2", TaskKind::kTimeout, 300));
  auto rows = m.take();
  // 写库失败期间又来了新变化
  m.add(tc("t1", TaskKind::kSent, 2));
  m.add(tc("t2", TaskKind::kDone, 301));
  m.restore(rows);
  rows = m.take();
  ASSERT_EQ(rows.size(), 2U);
  const auto& a = rows[0].task_id == "t1" ? rows[0] : rows[1];
  const auto& b = rows[0].task_id == "t1" ? rows[1] : rows[0];
  EXPECT_TRUE(a.has_alloc);
  EXPECT_EQ(a.input_bytes, 7U);
  EXPECT_EQ(a.state, TaskKind::kSent);
  EXPECT_EQ(*a.t_alloc, 1);
  EXPECT_EQ(*a.t_sent, 2);
  EXPECT_TRUE(b.has_alloc);
  EXPECT_EQ(b.state, TaskKind::kDone);  // 迟到的完成覆盖放回来的 timeout
  EXPECT_EQ(*b.t_done, 301);
}

TEST(TaskMerger, OverrideRules) {
  EXPECT_TRUE(task_state_overrides(TaskKind::kAllocated, TaskKind::kSent));
  EXPECT_FALSE(task_state_overrides(TaskKind::kReceived, TaskKind::kSent));
  EXPECT_TRUE(task_state_overrides(TaskKind::kTimeout, TaskKind::kDone));
  EXPECT_TRUE(task_state_overrides(TaskKind::kTimeout, TaskKind::kFailed));
  EXPECT_FALSE(task_state_overrides(TaskKind::kVoid, TaskKind::kDone));
  EXPECT_FALSE(task_state_overrides(TaskKind::kDone, TaskKind::kTimeout));
}

// ---------- 事件 ----------

StateChange chg(NodeMode from, NodeMode to, const std::string& reason = "") {
  StateChange c;
  c.node_id = "w1";
  c.from = from;
  c.to = to;
  c.reason = reason;
  c.since_wall_ms = kBase;
  return c;
}

TEST(Events, FromStateChange) {
  auto e = events_from_change(chg(NodeMode::kSlowStart, NodeMode::kSlowStart), "10.0.0.1:50052", 4);
  ASSERT_EQ(e.size(), 1U);
  EXPECT_EQ(e[0].kind, "slow_start");
  EXPECT_EQ(e[0].ts_ms, kBase);
  EXPECT_EQ(e[0].detail, R"({"from":"new","addr":"10.0.0.1:50052","ncpu":4})");
  EXPECT_EQ(events_from_change(chg(NodeMode::kEvicted, NodeMode::kSlowStart))[0].detail,
            R"({"from":"evicted"})");
  EXPECT_EQ(events_from_change(chg(NodeMode::kRescue, NodeMode::kSlowStart))[0].detail,
            R"({"from":"rescue"})");
  e = events_from_change(chg(NodeMode::kSlowStart, NodeMode::kNormal));
  EXPECT_EQ(e[0].kind, "normal");
  EXPECT_EQ(e[0].detail, "{}");
  e = events_from_change(chg(NodeMode::kNormal, NodeMode::kEvicted, "lost"));
  EXPECT_EQ(e[0].kind, "evict");
  EXPECT_EQ(e[0].detail, R"({"from":"normal","reason":"lost"})");
  auto c = chg(NodeMode::kNormal, NodeMode::kRescue);
  c.detail_json = R"({"prob":0.1,"candidates":2,"timeout_count":4,"backoff_s":300})";
  e = events_from_change(c);
  EXPECT_EQ(e[0].kind, "rescue_start");
  EXPECT_EQ(e[0].detail, c.detail_json);
}

TEST(Events, RescueEnd) {
  RescueEnd r;
  r.node_id = "w1";
  r.ts_ms = kBase;
  r.result = "reached";
  r.ms = 2200;
  r.batches_ok = 21;
  r.batches_fail = 1;
  r.sent = 2200;
  r.ok = 2190;
  r.timeouts = 10;
  r.prob = 0.4;
  r.target = 0.25;
  r.t_before = 8;
  r.t_after = 0.87;
  r.to = "slow_start";
  const auto e = event_from_rescue_end(r);
  EXPECT_EQ(e.kind, "rescue_end");
  EXPECT_EQ(e.ts_ms, kBase);
  EXPECT_EQ(e.detail,
            R"({"result":"reached","ms":2200,"batches_ok":21,"batches_fail":1,"sent":2200,"ok":2190,)"
            R"("timeouts":10,"late":0,"events":0,"prob":0.4,"target":0.25,"t_before":8,"t_after":0.87,)"
            R"("to":"slow_start"})");
}

// ---------- SQL 生成（不连库）----------

TEST(MysqlSql, Statements) {
  MysqlQueue q(16);
  MysqlWriter::Options o;
  o.db.enabled = false;
  MysqlWriter w(q, o);

  MetricsRow m;
  m.node_id = "w1";
  m.bucket_start_ms = kBase;
  m.cpu_avg = 0.5;
  m.cpu_max = 0.9;
  m.mem_avail_min = 0.3;
  m.score_avg = 0.7;
  m.samples = 300;
  m.net_rtt_avg_ms = 3;
  m.net_timeout_rate = 0.2;
  m.net_retrans_rate = std::nan("");  // NaN 写 NULL
  const auto sm = w.sql_metrics({m});
  EXPECT_NE(sm.find("('w1','2027-01-15 08:00:00',0.5,0.9,NULL,0.3,3,0.2,NULL,0.7,300)"),
            std::string::npos)
      << sm;
  EXPECT_NE(sm.find("ON DUPLICATE KEY UPDATE"), std::string::npos);

  NodeEvent e;
  e.node_id = "w'1";
  e.kind = "normal";
  e.ts_ms = kBase + 5;
  e.detail = "{}";
  e.id = "1800000000000-0";
  NodeEvent e2 = e;
  e2.kind = "evict";
  e2.detail = R"({"from":"normal","reason":"lost"})";
  e2.id = "";
  const auto se = w.sql_events({e, e2});
  EXPECT_NE(se.find(R"(('w\'1','2027-01-15 08:00:00.005','normal','{\"event_id\":\"1800000000000-0\"}'))"),
            std::string::npos)
      << se;
  EXPECT_NE(se.find(R"('{\"event_id\":\"\",\"from\":\"normal\",\"reason\":\"lost\"}')"),
            std::string::npos)
      << se;

  const auto sn = w.sql_nodes({NodeRow{"w1", "10.0.0.1:50052", 4, kBase}});
  EXPECT_NE(sn.find("('w1','10.0.0.1:50052',4,'2027-01-15 08:00:00')"), std::string::npos) << sn;

  TaskRow t;
  t.task_id = "abc-1";
  t.has_alloc = true;
  t.node_id = "w1";
  t.input_bytes = 10;
  t.state = TaskKind::kSent;
  t.t_alloc = kBase;
  t.t_sent = kBase + 1;
  const auto si = w.sql_task_insert({t});
  EXPECT_NE(si.find("('abc-1','w1',10,'sent','2027-01-15 08:00:00.000','2027-01-15 08:00:00.001',NULL,NULL)"),
            std::string::npos)
      << si;
  EXPECT_NE(si.find("t_sent=COALESCE(t_sent, VALUES(t_sent))"), std::string::npos);
  t.has_alloc = false;
  t.state = TaskKind::kDone;
  t.t_done = kBase + 9;
  const auto su = w.sql_task_update(t);
  EXPECT_EQ(su.find("UPDATE task SET state = IF("), 0U) << su;
  EXPECT_NE(su.find("state = 'timeout' AND 'done' IN ('done','failed')"), std::string::npos) << su;
  EXPECT_NE(su.find("WHERE task_id='abc-1'"), std::string::npos);
}

TEST(MysqlWriter, DisabledDropsEverythingWithoutError) {
  MysqlQueue q(16);
  MysqlWriter::Options o;
  o.db.enabled = false;
  MysqlWriter w(q, o);
  w.consume({pt("w1", kBase, 0.1, 0.5, 1), tc("t1", TaskKind::kAllocated, 1, "w1")});
  w.flush(kBase + 400 * S, 0, false);
  EXPECT_EQ(w.errors(), 0U);
  EXPECT_EQ(w.rows_written(), 0U);
}

// ---------- 选中概率与任务变化的生产者 ----------

lm::Sample sample(const std::string& id, uint64_t seq, float cpu, float mem) {
  lm::Sample s;
  s.set_node_id(id);
  s.set_seq(seq);
  s.add_core_busy(cpu);
  s.set_cpu_busy(cpu);
  s.set_mem_avail_ratio(mem);
  s.set_runq_p99_us(50.0F);
  auto* n = s.add_nics();
  n->set_name("eth0");
  n->set_rtt_ms(1);
  return s;
}

TEST(NodeTableProb, TwoNodesSixtyForty) {
  NodeTable::Options o;
  o.breaker.slow_start_ms = 1;
  NodeTable t(o);
  // w1：cpu 0.2 → 满分 1.0；w2：cpu 0.9（0 分）、mem 0.1（0 分）→ 0.3 + 0.2 = 0.5…
  // 用 cpu 调分数：总分 = 0.3·cpu_s + 0.7。cpu_s(w2) = 1/3 → 0.8；目标比例 0.6/0.4 需要 w2 = 2/3·w1
  // 取 w1 = 0.9（cpu_s 2/3），w2 = 0.6：cpu 0.9（0）、mem 0.3（0.5）→ 0 + 0.3 + 0.1 + 0.2 = 0.6
  t.apply(sample("w1", 1, static_cast<float>(0.9 - 0.7 * (2.0 / 3)), 0.8F), "10.0.0.1", 0, 0, nullptr);
  t.apply(sample("w2", 1, 0.9F, 0.3F), "10.0.0.2", 0, 0, nullptr);
  std::vector<StateChange> ch;
  t.tick_states(1, 1, &ch, nullptr);
  ASSERT_EQ(*t.mode("w1"), NodeMode::kNormal);
  const double s1 = t.get("w1")->score.total, s2 = t.get("w2")->score.total;
  EXPECT_NEAR(s1, 0.9, 1e-3);
  EXPECT_NEAR(s2, 0.6, 1e-3);
  EXPECT_NEAR(t.get("w1")->prob, 0.6, 1e-3);
  EXPECT_NEAR(t.get("w2")->prob, 0.4, 1e-3);
  // 随下一条上报带出
  AcceptedSample acc;
  t.apply(sample("w1", 2, static_cast<float>(0.9 - 0.7 * (2.0 / 3)), 0.8F), "10.0.0.1", 2, 2, &acc);
  EXPECT_NEAR(acc.prob, 0.6, 1e-3);
  // 失联节点概率为 0
  t.tick_states(10 * S, 10 * S, &ch, nullptr);
  EXPECT_EQ(t.get("w2")->prob, 0);
}

TEST(NodeTableProb, RescueStartDetail) {
  NodeTable::Options o;
  o.breaker.slow_start_ms = 1;
  NodeTable t(o);
  t.apply(sample("w1", 1, 0.1F, 0.8F), "10.0.0.1", 0, 0, nullptr);
  t.apply(sample("w2", 1, 0.1F, 0.8F), "10.0.0.2", 0, 0, nullptr);
  std::vector<StateChange> ch;
  t.tick_states(1, 1, &ch, nullptr);
  for (int i = 0; i < 4; ++i) {
    const std::string tid = "x" + std::to_string(i);
    t.allocate(1, 0.0, tid, 0);
    t.on_event(tid, lm::TaskEvent::RECEIVED, 1);
  }
  t.tick(301 * S + 1, nullptr);
  t.apply(sample("w1", 2, 0.1F, 0.8F), "10.0.0.1", 301 * S + 1, 0, nullptr);
  t.apply(sample("w2", 2, 0.1F, 0.8F), "10.0.0.2", 301 * S + 1, 0, nullptr);
  ch.clear();
  std::vector<RescueStart> rs;
  t.tick_states(301 * S + 1, 0, &ch, &rs);
  ASSERT_EQ(rs.size(), 1U);
  ASSERT_FALSE(ch.empty());
  EXPECT_EQ(ch.back().to, NodeMode::kRescue);
  EXPECT_NE(ch.back().detail_json.find(R"("candidates":2)"), std::string::npos) << ch.back().detail_json;
  EXPECT_NE(ch.back().detail_json.find(R"("timeout_count":4)"), std::string::npos);
  EXPECT_NE(ch.back().detail_json.find(R"("backoff_s":300)"), std::string::npos);
}

TEST(TaskProducers, LateEventStillEmitsDone) {
  NodeTable t({});
  BoundedQueue<AcceptedSample> q(4);
  CollectorService c(t, q);
  std::vector<TaskChange> got;
  c.set_task_sink([&](TaskChange x) { got.push_back(std::move(x)); });
  lm::TaskEvent ev;
  ev.set_task_id("unknown-1");
  ev.set_kind(lm::TaskEvent::DONE);
  lm::Ack ack;
  ASSERT_TRUE(c.OnTaskEvent(nullptr, &ev, &ack).ok());
  ev.set_kind(lm::TaskEvent::FAILED);
  ASSERT_TRUE(c.OnTaskEvent(nullptr, &ev, &ack).ok());
  ev.set_kind(lm::TaskEvent::RECEIVED);
  ASSERT_TRUE(c.OnTaskEvent(nullptr, &ev, &ack).ok());
  ASSERT_EQ(got.size(), 3U);
  EXPECT_EQ(got[0].task_id, "unknown-1");
  EXPECT_EQ(got[0].kind, TaskKind::kDone);
  EXPECT_EQ(got[1].kind, TaskKind::kFailed);
  EXPECT_EQ(got[2].kind, TaskKind::kReceived);
  EXPECT_GT(got[0].ts_ms, 0);
}

TEST(TaskProducers, SchedulerLoopTimeouts) {
  NodeTable::Options o;
  NodeTable t(o);
  t.apply(sample("w1", 1, 0.1F, 0.8F), "10.0.0.1", 0, 0, nullptr);
  t.allocate(0, 0.0, "a", 0);                      // 不 MarkSent → 作废
  t.allocate(0, 0.0, "b", 0);
  t.mark_sent("b", 0);                             // 已发送未收到 → 未确认
  t.allocate(0, 0.0, "c", 0);
  t.on_event("c", lm::TaskEvent::RECEIVED, 0);     // 执行超时
  SchedulerLoop loop(t, {});
  std::map<std::string, TaskKind> got;
  loop.set_task_sink([&](TaskChange x) { got[x.task_id] = x.kind; });
  loop.tick_once(31 * S);
  loop.tick_once(301 * S);
  ASSERT_EQ(got.size(), 3U);
  EXPECT_EQ(got["a"], TaskKind::kVoid);
  EXPECT_EQ(got["b"], TaskKind::kTimeout);
  EXPECT_EQ(got["c"], TaskKind::kTimeout);
}

TEST(TaskProducers, AllocateAndMarkSent) {
  NodeTable t({});
  t.apply(sample("w1", 1, 0.1F, 0.8F), "10.0.0.1", lm::mono_ms(), 0, nullptr);
  SchedulerService svc(t, {});
  std::vector<TaskChange> got;
  svc.set_task_sink([&](TaskChange x) { got.push_back(std::move(x)); });
  lm::AllocateReq req;
  req.set_client_id("c1");
  req.set_input_bytes(123);
  lm::AllocateResp resp;
  ASSERT_TRUE(svc.Allocate(nullptr, &req, &resp).ok());
  lm::TaskRef ref;
  ref.set_task_id(resp.task_id());
  lm::Ack ack;
  ASSERT_TRUE(svc.MarkSent(nullptr, &ref, &ack).ok());
  ref.set_task_id("nope");
  EXPECT_FALSE(svc.MarkSent(nullptr, &ref, &ack).ok());  // 未知任务不发
  ASSERT_EQ(got.size(), 2U);
  EXPECT_EQ(got[0].kind, TaskKind::kAllocated);
  EXPECT_EQ(got[0].task_id, resp.task_id());
  EXPECT_EQ(got[0].node_id, "w1");
  EXPECT_EQ(got[0].input_bytes, 123U);
  EXPECT_EQ(got[1].kind, TaskKind::kSent);
}

}  // namespace
