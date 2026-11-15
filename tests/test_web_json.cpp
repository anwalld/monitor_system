// 看板接口纯逻辑：降采样、节点与事件 JSON、token、Stream id 比较。

#include <gtest/gtest.h>

#include "api/web_json.h"

using namespace lm::manager;

namespace {

lm::Sample smp(int64_t ts, float cpu, std::optional<float> p99, int nics) {
  lm::Sample s;
  s.set_node_id("w1");
  s.set_ts_ms(ts);
  s.set_cpu_busy(cpu);
  s.set_mem_avail_ratio(0.5F);
  if (p99) s.set_runq_p99_us(*p99);
  for (int i = 0; i < nics; ++i) {
    auto* n = s.add_nics();
    n->set_name("eth" + std::to_string(i));
    n->set_rtt_ms(static_cast<float>(i + 1));  // 1、2 → 平均 1.5
    n->set_timeout_rate(0.1F * static_cast<float>(i));
    n->set_rx_bps(1000);
  }
  return s;
}

TEST(Downsample, OneHourTo720Points) {
  std::vector<lm::Sample> v;
  const int64_t t0 = 1'800'000'000'000;
  for (int i = 0; i < 3600; ++i) v.push_back(smp(t0 + i * 1000, 0.5F, 100.0F, 1));
  const auto pts = downsample(v, 5000);
  ASSERT_EQ(pts.size(), 720U);
  EXPECT_EQ(pts[0].ts_ms, t0);
  EXPECT_EQ(pts[1].ts_ms - pts[0].ts_ms, 5000);
  EXPECT_EQ(pts[0].n, 5);
}

TEST(Downsample, BucketRules) {
  const int64_t t0 = 1'800'000'000'000;
  // 乱序输入；P99 取最大；网络先对网卡平均；缺 runq 的桶为空；空桶不输出
  std::vector<lm::Sample> v = {smp(t0 + 3000, 0.6F, 300.0F, 2), smp(t0 + 1000, 0.2F, 100.0F, 0),
                               smp(t0 + 20000, 0.1F, std::nullopt, 0)};
  const auto pts = downsample(v, 5000);
  ASSERT_EQ(pts.size(), 2U);
  EXPECT_NEAR(pts[0].cpu, 0.4, 1e-6);
  EXPECT_NEAR(*pts[0].runq_p99_us, 300, 1e-6);
  EXPECT_NEAR(*pts[0].rtt_ms, 1.5, 1e-6);
  EXPECT_NEAR(*pts[0].timeout_rate, 0.05, 1e-6);
  EXPECT_NEAR(pts[0].rx_bps, 1000, 1e-6);  // 两块网卡 2000 / 2 个样本
  EXPECT_EQ(pts[1].ts_ms, t0 + 20000);
  EXPECT_FALSE(pts[1].runq_p99_us);
  EXPECT_FALSE(pts[1].rtt_ms);
  const auto j = series_json("w1", 5000, pts);
  EXPECT_NE(j.find(R"("step_ms":5000)"), std::string::npos);
  EXPECT_NE(j.find(R"("runq_p99_us":null)"), std::string::npos) << j;
}

TEST(WebJson, NodeObject) {
  NodeView v;
  v.st.node_id = "w1";
  v.st.addr = "10.0.0.1:50052";
  v.st.ncpu = 4;
  v.st.prob = 0.41;
  v.st.score.total = 0.82;
  v.st.score.best_nic = "ens160";
  v.st.latest = smp(1, 0.33F, std::nullopt, 1);
  v.online = true;
  v.inflight.inflight_tasks = 2;
  const auto j = node_json(v);
  for (const char* want : {R"("id":"w1")", R"("state":"slow_start")", R"("prob":0.41)",
                           R"("score":0.82)", R"("runq_p99_us":null)", R"("online":true)",
                           R"("best_nic":"ens160")", R"("inflight":2)", R"("name":"eth0")"}) {
    EXPECT_NE(j.find(want), std::string::npos) << want << " in " << j;
  }
  EXPECT_NE(j.find(R"("f_pred":1)"), std::string::npos) << j;
  EXPECT_NE(j.find(R"("pred_cpu":null)"), std::string::npos) << j;
  v.st.latest.set_runq_p99_us(180);
  EXPECT_NE(node_json(v).find(R"("runq_p99_us":180)"), std::string::npos);
  v.f_pred = 0.75;
  v.pred_cpu = 0.85;
  EXPECT_NE(node_json(v).find(R"("f_pred":0.75)"), std::string::npos);
  EXPECT_NE(node_json(v).find(R"("pred_cpu":0.85)"), std::string::npos);
  EXPECT_EQ(nodes_array_json({}), "[]");
}

TEST(WebJson, EventObject) {
  StreamFields f = {{"node", "w2"}, {"kind", "evict"}, {"ts_ms", "1731400001234"},
                    {"detail", R"({"from":"normal","reason":"lost"})"}};
  EXPECT_EQ(event_json("1731400001234-0", f, true),
            R"({"type":"event","id":"1731400001234-0","node":"w2","kind":"evict","ts":1731400001234,)"
            R"("detail":{"from":"normal","reason":"lost"}})");
  // detail 不是对象：当字符串；缺 ts_ms：取 id 的毫秒部分
  StreamFields g = {{"node", "w2"}, {"kind", "x"}, {"detail", "oops"}};
  EXPECT_EQ(event_json("5-1", g, false), R"({"id":"5-1","node":"w2","kind":"x","ts":5,"detail":"oops"})");
}

TEST(WebJson, TokenAndIds) {
  EXPECT_TRUE(token_ok("", "anything"));
  EXPECT_TRUE(token_ok("abc", "abc"));
  EXPECT_FALSE(token_ok("abc", "abd"));
  EXPECT_FALSE(token_ok("abc", "ab"));
  EXPECT_FALSE(token_ok("abc", ""));
  EXPECT_EQ(bearer_token("Bearer xyz"), "xyz");
  EXPECT_EQ(bearer_token("Basic xyz"), "");
  EXPECT_TRUE(stream_id_less("1-9", "2-0"));
  EXPECT_TRUE(stream_id_less("10-1", "10-2"));
  EXPECT_FALSE(stream_id_less("10-2", "9-5"));
  EXPECT_TRUE(stream_id_less("bad", "0-1"));
}

}  // namespace
