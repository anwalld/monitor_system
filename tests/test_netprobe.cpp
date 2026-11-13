#include <gtest/gtest.h>

#include <arpa/inet.h>

#include <cmath>
#include <cstring>
#include <set>

#include "collector/collector.h"
#include "icmp.h"
#include "nic_config.h"
#include "nic_monitor.h"
#include "probe_state.h"
#include "retrans.h"
#include "traffic.h"

using namespace lm::worker;

namespace {

uint32_t ip(const char* s) {
  uint32_t a = 0;
  EXPECT_TRUE(parse_ipv4(s, a));
  return a;
}

// ---------- 配置 ----------

const std::set<std::string> kNics = {"ens160", "ens192"};
bool fake_exists(const std::string& n) { return kNics.count(n) > 0; }

TEST(NetConfig, ParsesExplicitNicsInOrder) {
  auto cfg = lm::Config::load_string(R"(
net:
  probe_interval_ms: 200
  probe_timeout_ms: 1000
  nics:
    - name: ens160
      targets: [172.16.255.2, 8.8.8.8]
    - name: ens192
      targets: [172.16.13.1]
)");
  const auto c = parse_net_config(cfg, fake_exists);
  ASSERT_EQ(c.nics.size(), 2U);
  EXPECT_EQ(c.nics[0].name, "ens160");
  EXPECT_EQ(c.nics[1].name, "ens192");
  ASSERT_EQ(c.nics[0].targets.size(), 2U);
  EXPECT_EQ(c.nics[0].targets[1], ip("8.8.8.8"));
  EXPECT_EQ(c.interval_ms, 200);
  EXPECT_EQ(c.timeout_ms, 1000);
}

TEST(NetConfig, DefaultsAndAutoWhenNoNics) {
  const auto c = parse_net_config(lm::Config::load_string("log_level: info\n"), fake_exists);
  EXPECT_TRUE(c.nics.empty());
  EXPECT_EQ(c.interval_ms, 200);
  EXPECT_EQ(c.timeout_ms, 1000);
}

void expect_config_error(const std::string& yaml, const std::string& needle) {
  try {
    parse_net_config(lm::Config::load_string(yaml), fake_exists);
    FAIL() << "expected ConfigError for: " << yaml;
  } catch (const lm::ConfigError& e) {
    EXPECT_NE(std::string(e.what()).find(needle), std::string::npos) << e.what();
  }
}

TEST(NetConfig, Errors) {
  expect_config_error("net:\n  nics:\n    - name: eth9\n      targets: [1.1.1.1]\n", "eth9");
  expect_config_error("net:\n  nics:\n    - name: ens160\n      targets: [1.2.3]\n", "1.2.3");
  expect_config_error(
      "net:\n  nics:\n    - name: ens160\n      targets: [1.1.1.1, 2.2.2.2, 3.3.3.3, 4.4.4.4]\n",
      "超过 3 个");
  expect_config_error("net:\n  nics:\n    - name: ens160\n      targets: []\n", "ens160");
  expect_config_error("net:\n  probe_interval_ms: 10\n", "probe_interval_ms");
  expect_config_error("net:\n  probe_interval_ms: 500\n  probe_timeout_ms: 200\n", "probe_timeout_ms");
}

// ---------- ICMP ----------

TEST(Icmp, ChecksumKnownVector) {
  // RFC 1071 示例：00 01 f2 03 f4 f5 f6 f7 → 校验和 0x220d
  const uint8_t d[] = {0x00, 0x01, 0xf2, 0x03, 0xf4, 0xf5, 0xf6, 0xf7};
  EXPECT_EQ(ntohs(inet_checksum(d, sizeof(d))), 0x220dU);
}

// 把请求改造成“带 IPv4 头的应答”
std::vector<uint8_t> as_reply(std::vector<uint8_t> icmp, const char* src) {
  icmp[0] = 0;  // echo reply
  icmp[2] = icmp[3] = 0;
  const uint16_t c = inet_checksum(icmp.data(), icmp.size());
  std::memcpy(&icmp[2], &c, 2);
  std::vector<uint8_t> pkt(20, 0);
  pkt[0] = 0x45;
  pkt[9] = 1;
  const uint32_t a = ip(src);
  std::memcpy(&pkt[12], &a, 4);
  pkt.insert(pkt.end(), icmp.begin(), icmp.end());
  return pkt;
}

TEST(Icmp, BuildAndParseReply) {
  const auto req = build_echo_request(0x1234, 77, 1, 123456789ULL);
  ASSERT_EQ(req.size(), kIcmpEchoLen);
  EXPECT_EQ(req[0], 8);
  EXPECT_EQ(inet_checksum(req.data(), req.size()), 0);  // 带校验和整体求和为 0
  const auto pkt = as_reply(req, "172.16.13.1");
  EchoReply r;
  ASSERT_TRUE(parse_echo_reply(pkt.data(), pkt.size(), r));
  EXPECT_EQ(r.ident, 0x1234);
  EXPECT_EQ(r.seq, 77);
  EXPECT_EQ(r.nic_index, 1U);
  EXPECT_EQ(r.src, ip("172.16.13.1"));
}

TEST(Icmp, RejectsBadPackets) {
  const auto req = build_echo_request(1, 2, 0, 0);
  auto pkt = as_reply(req, "1.1.1.1");
  EchoReply r;
  // 类型不是应答
  auto bad = pkt;
  bad[20] = 8;
  EXPECT_FALSE(parse_echo_reply(bad.data(), bad.size(), r));
  // 截断
  EXPECT_FALSE(parse_echo_reply(pkt.data(), 30, r));
  // 校验和错误
  bad = pkt;
  bad[30] ^= 0xff;
  EXPECT_FALSE(parse_echo_reply(bad.data(), bad.size(), r));
  // 不是 ICMP
  bad = pkt;
  bad[9] = 6;
  EXPECT_FALSE(parse_echo_reply(bad.data(), bad.size(), r));
}

// ---------- 探测状态 ----------

constexpr int64_t kMs = 1000000;

TEST(ProbeState, SpecExampleMeanJitterTimeoutRate) {
  ProbeState st(1000 * kMs);
  const uint32_t t = ip("10.0.0.1");
  for (uint16_t i = 1; i <= 10; ++i) st.on_sent(i, t, 0);
  for (uint16_t i = 1; i <= 9; ++i) ASSERT_TRUE(st.on_reply(i, t, i * kMs));
  EXPECT_EQ(st.expire(1001 * kMs), 1);  // seq 10 超时
  const auto p = st.take();
  EXPECT_EQ(p.ok, 9U);
  EXPECT_EQ(p.timeouts, 1U);
  EXPECT_NEAR(p.rtt_ms, 5.0, 1e-9);
  EXPECT_NEAR(p.jitter_ms, 2.582, 1e-3);
  EXPECT_NEAR(p.timeout_rate, 0.1, 1e-12);
  const auto empty = st.take();  // 清零
  EXPECT_EQ(empty.ok + empty.timeouts, 0U);
  EXPECT_EQ(empty.rtt_ms, 0.0);
  EXPECT_EQ(empty.timeout_rate, 0.0);
}

TEST(ProbeState, UnreachableTargetIsAllTimeouts) {
  ProbeState st(1000 * kMs);
  const uint32_t t = ip("10.0.0.1");
  int64_t now = 0;
  uint16_t seq = 0;
  for (int s = 0; s < 3; ++s) {  // 3 秒，每 200ms 发一次，每 100ms 检查超时
    for (int k = 0; k < 10; ++k, now += 100 * kMs) {
      if (k % 2 == 0) st.on_sent(seq++, t, now);
      st.expire(now);
    }
  }
  st.take();  // 丢掉前 1 秒的“尚未到期”阶段
  now += 1100 * kMs;
  st.expire(now);
  const auto p = st.take();
  EXPECT_GT(p.timeouts, 0U);
  EXPECT_EQ(p.ok, 0U);
  EXPECT_EQ(p.timeout_rate, 1.0);
  EXPECT_EQ(p.rtt_ms, 0.0);
  EXPECT_EQ(p.jitter_ms, 0.0);
}

TEST(ProbeState, LateReplyCountedOnceAsTimeout) {
  ProbeState st(1000 * kMs);
  const uint32_t t = ip("10.0.0.1");
  st.on_sent(5, t, 0);
  EXPECT_EQ(st.expire(1100 * kMs), 1);
  EXPECT_FALSE(st.on_reply(5, t, 1200 * kMs));  // 迟到：丢弃
  const auto p = st.take();
  EXPECT_EQ(p.ok, 0U);
  EXPECT_EQ(p.timeouts, 1U);
}

TEST(ProbeState, WrongSourceAndSendFailure) {
  ProbeState st(1000 * kMs);
  st.on_sent(1, ip("10.0.0.1"), 0);
  EXPECT_FALSE(st.on_reply(1, ip("10.0.0.9"), 5 * kMs));  // 来源不对
  EXPECT_TRUE(st.on_reply(1, ip("10.0.0.1"), 5 * kMs));
  st.on_send_failed();
  const auto p = st.take();
  EXPECT_EQ(p.ok, 1U);
  EXPECT_EQ(p.timeouts, 1U);
  EXPECT_DOUBLE_EQ(p.timeout_rate, 0.5);
}

TEST(ProbeState, OutOfOrderRepliesMatchBySeq) {
  ProbeState st(1000 * kMs);
  const uint32_t t = ip("10.0.0.1");
  st.on_sent(1, t, 0);
  st.on_sent(2, t, 200 * kMs);
  EXPECT_TRUE(st.on_reply(2, t, 210 * kMs));
  EXPECT_EQ(st.pending(), 1U);  // seq 1 还在等
  EXPECT_TRUE(st.on_reply(1, t, 220 * kMs));
  EXPECT_EQ(st.pending(), 0U);
}

// ---------- 重传与流量 ----------

TEST(Retrans, SpecExample) {
  const uint32_t a = ip("172.16.13.128");
  RetransTracker tr({{a, 1}}, 2);
  tr.update({{1, a, 100, 1000}, {2, a, 50, 500}});
  const auto r = tr.update({{1, a, 118, 1900}, {2, a, 52, 600}});
  EXPECT_DOUBLE_EQ(r[1], 0.02);
  EXPECT_DOUBLE_EQ(r[0], 0.0);
}

TEST(Retrans, NewConnBaselineVanishedRemovedAndUnmappedIgnored) {
  const uint32_t a = ip("10.0.0.2");
  const uint32_t lo = ip("127.0.0.1");
  RetransTracker tr({{a, 0}}, 1);
  tr.update({});
  auto r = tr.update({{7, a, 50, 5000}, {8, lo, 999, 1000}});  // 新连接：只记基线；回环忽略
  EXPECT_EQ(r[0], 0.0);
  r = tr.update({{7, a, 60, 6000}});
  EXPECT_DOUBLE_EQ(r[0], 0.01);
  tr.update({});                       // 连接 7 消失
  r = tr.update({{7, a, 70, 7000}});  // 同一 cookie 再出现：当作新连接
  EXPECT_EQ(r[0], 0.0);
}

TEST(Retrans, U32Wraparound) {
  const uint32_t a = ip("10.0.0.2");
  RetransTracker tr({{a, 0}}, 1);
  tr.update({{1, a, 0xFFFFFFF0U, 0xFFFFFF00U}});
  const auto r = tr.update({{1, a, 0x0000000AU, 0x00000064U}});  // Δ重传 26，Δ段 356
  EXPECT_NEAR(r[0], 26.0 / 356.0, 1e-12);
}

TEST(RateCounter, BitsPerSecondBaselineAndReset) {
  RateCounter rc;
  EXPECT_EQ(rc.update(1000, 0), 0U);
  EXPECT_EQ(rc.update(1000 + 125000, 1000000000LL), 1000000U);  // 125000 B/s = 1 Mbps
  EXPECT_EQ(rc.update(10, 2000000000LL), 0U);                   // 计数倒退
  EXPECT_EQ(rc.update(10 + 250, 2500000000LL), 4000U);
}

// ---------- NicMonitor 失败沿用 ----------

TEST(NicMonitor, AssemblesStatsAndKeepsLastOnFailure) {
  const uint32_t a = ip("10.0.0.2");
  int tick = 0;
  bool tcp_fail = false;
  bool bytes_fail = false;
  NicMonitor::Sources src;
  src.ip_to_nic = {{a, 0}};
  src.probe = [] {
    ProbePeriod p;
    p.rtt_ms = 3.5;
    p.jitter_ms = 0.5;
    p.timeout_rate = 0.25;
    return std::vector<ProbePeriod>{p};
  };
  src.tcp = [&]() -> std::optional<std::vector<TcpConnSample>> {
    if (tcp_fail) return std::nullopt;
    return std::vector<TcpConnSample>{{1, a, static_cast<uint32_t>(tick), static_cast<uint32_t>(tick * 10)}};
  };
  src.bytes = [&](std::size_t, bool rx) -> std::optional<uint64_t> {
    if (bytes_fail) return std::nullopt;
    return static_cast<uint64_t>(tick) * (rx ? 1000 : 2000);
  };
  src.now_ns = [&] { return static_cast<int64_t>(tick) * 1000000000LL; };
  NicMonitor m({"ens192"}, src);

  tick = 1;
  auto s = m.collect();  // 基线
  ASSERT_EQ(s.size(), 1U);
  EXPECT_EQ(s[0].name(), "ens192");
  EXPECT_FLOAT_EQ(s[0].rtt_ms(), 3.5F);
  EXPECT_FLOAT_EQ(s[0].timeout_rate(), 0.25F);
  EXPECT_EQ(s[0].tx_bps(), 0U);

  tick = 2;
  s = m.collect();
  EXPECT_FLOAT_EQ(s[0].retrans_rate(), 0.1F);  // Δ1 / Δ10
  EXPECT_EQ(s[0].rx_bps(), 8000U);
  EXPECT_EQ(s[0].tx_bps(), 16000U);

  tick = 3;
  tcp_fail = bytes_fail = true;
  s = m.collect();  // 沿用上次
  EXPECT_FLOAT_EQ(s[0].retrans_rate(), 0.1F);
  EXPECT_EQ(s[0].tx_bps(), 16000U);
  EXPECT_FLOAT_EQ(s[0].rtt_ms(), 3.5F);  // 探测结果照常
}

// ---------- Collector 写入 nics ----------

class OneCpu : public CpuSource {
 public:
  bool read(CpuSnapshot& out) override {
    ++n_;
    out.cores = {{0, n_ * 50, n_ * 100}};
    return true;
  }

 private:
  uint64_t n_ = 0;
};

TEST(CollectorNics, WritesNicsInOrderOrNone) {
  SampleRing ring;
  MetricReaders r{[] { return 0.5; }, [](int64_t) { return 0.1; }, nullptr, [] {
                    std::vector<lm::NicStat> v(2);
                    v[0].set_name("ens160");
                    v[1].set_name("ens192");
                    v[1].set_rtt_ms(0.5F);
                    return v;
                  }};
  Collector c("w1", std::make_unique<OneCpu>(), std::move(r), ring, std::chrono::milliseconds(1000));
  c.collect_once();
  ASSERT_TRUE(c.collect_once());
  lm::Sample s;
  ASSERT_TRUE(ring.take_latest(s));
  ASSERT_EQ(s.nics_size(), 2);
  EXPECT_EQ(s.nics(0).name(), "ens160");
  EXPECT_EQ(s.nics(1).name(), "ens192");
  EXPECT_FLOAT_EQ(s.nics(1).rtt_ms(), 0.5F);

  SampleRing ring2;
  Collector d("w1", std::make_unique<OneCpu>(), {[] { return 0.5; }, [](int64_t) { return 0.1; }},
              ring2, std::chrono::milliseconds(1000));
  d.collect_once();
  ASSERT_TRUE(d.collect_once());
  ASSERT_TRUE(ring2.take_latest(s));
  EXPECT_EQ(s.nics_size(), 0);
}

TEST(ProberStub, ThrowsOffLinux) {
#if !defined(__linux__)
  NetProbeConfig cfg;
  EXPECT_THROW(NicMonitor::create(cfg, {{"en0", {1}, {"1.1.1.1"}}}), std::runtime_error);
#else
  GTEST_SKIP() << "Linux 上由虚拟机验收覆盖";
#endif
}

}  // namespace
