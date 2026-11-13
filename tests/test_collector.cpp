#include <gtest/gtest.h>

#include <thread>
#include <vector>

#include "collector/collector.h"

using namespace lm::worker;

namespace {

// 每次读数每核 total +100、idle +50，忙碌度恒为 0.5
class FakeCpu : public CpuSource {
 public:
  explicit FakeCpu(int ncpu) : ncpu_(ncpu) {}
  bool read(CpuSnapshot& out) override {
    if (fail_next) {
      fail_next = false;
      return false;
    }
    if (repeat_next) {
      repeat_next = false;
    } else {
      ++n_;
    }
    out.cores.clear();
    for (int i = 0; i < ncpu_; ++i) out.cores.push_back({static_cast<uint32_t>(i), n_ * 50, n_ * 100});
    return true;
  }
  bool fail_next = false;
  bool repeat_next = false;  // 下一次返回与上次相同的读数（模拟读到同一次共享页刷新）

 private:
  int ncpu_;
  uint64_t n_ = 0;
};

TEST(Collector, FirstCollectIsBaselineThenSamples) {
  SampleRing ring;
  auto cpu = std::make_unique<FakeCpu>(4);
  Collector c("w1", std::move(cpu), {[] { return 0.6; }, [](int64_t) { return 0.2; }}, ring,
              std::chrono::milliseconds(1000));
  EXPECT_FALSE(c.collect_once());
  ASSERT_TRUE(c.collect_once());
  lm::Sample s;
  ASSERT_TRUE(ring.take_latest(s));
  EXPECT_EQ(s.node_id(), "w1");
  EXPECT_EQ(s.seq(), 1U);
  EXPECT_EQ(s.core_busy_size(), 4);
  EXPECT_FLOAT_EQ(s.cpu_busy(), 0.5F);
  EXPECT_FLOAT_EQ(s.mem_avail_ratio(), 0.6F);
  EXPECT_FLOAT_EQ(s.disk_util(), 0.2F);
  EXPECT_FALSE(s.has_runq_p99_us());  // 没有 runq 读取函数时不设置调度延迟
  EXPECT_EQ(s.nics_size(), 0);
}

TEST(Collector, SameCpuDataStillPublishesWithLastBusy) {
  SampleRing ring;
  auto cpu = std::make_unique<FakeCpu>(2);
  FakeCpu* raw = cpu.get();
  Collector c("w1", std::move(cpu), {[] { return 0.6; }, [](int64_t) { return 0.2; }}, ring,
              std::chrono::milliseconds(1000));
  EXPECT_FALSE(c.collect_once());  // 基线
  raw->repeat_next = true;
  EXPECT_FALSE(c.collect_once());  // 基线后又是同一份数据，还没有可沿用的值
  ASSERT_TRUE(c.collect_once());
  raw->repeat_next = true;
  ASSERT_TRUE(c.collect_once());  // 同一份数据：照常出样本
  lm::Sample s;
  ASSERT_TRUE(ring.take_latest(s));
  EXPECT_EQ(s.seq(), 2U);
  EXPECT_FLOAT_EQ(s.cpu_busy(), 0.5F);
  EXPECT_EQ(s.core_busy_size(), 2);
  EXPECT_EQ(c.samples_published(), 2U);
}

TEST(Collector, RunqStatesMapToOptionalFields) {
  SampleRing ring;
  std::vector<RunqResult> script;
  RunqResult ok;
  ok.status = RunqResult::kOk;
  ok.stats.p50_us = 12.5F;
  ok.stats.p99_us = 850.0F;
  ok.stats.drop_rate = 0.0F;
  RunqResult base;
  base.status = RunqResult::kBaseline;
  RunqResult fail;
  fail.status = RunqResult::kFailed;
  RunqResult ok2 = ok;
  ok2.stats.p99_us = 2000.0F;
  // 采集 1 只建 CPU 基线（不出样本）；之后依次：基线、成功、失败（沿用）、成功
  script = {base, base, ok, fail, ok2};
  std::size_t i = 0;
  MetricReaders readers{[] { return 0.5; }, [](int64_t) { return 0.1; },
                        [&] { return script[std::min(i++, script.size() - 1)]; }};
  Collector c("w1", std::make_unique<FakeCpu>(1), std::move(readers), ring,
              std::chrono::milliseconds(1000));
  lm::Sample s;
  EXPECT_FALSE(c.collect_once());

  ASSERT_TRUE(c.collect_once());
  ASSERT_TRUE(ring.take_latest(s));
  EXPECT_FALSE(s.has_runq_p99_us());  // runq 还在基线

  ASSERT_TRUE(c.collect_once());
  ASSERT_TRUE(ring.take_latest(s));
  ASSERT_TRUE(s.has_runq_p99_us());
  EXPECT_FLOAT_EQ(s.runq_p50_us(), 12.5F);
  EXPECT_FLOAT_EQ(s.runq_p99_us(), 850.0F);
  EXPECT_TRUE(s.has_runq_drop_rate());  // 值为 0 也被设置
  EXPECT_EQ(s.runq_drop_rate(), 0.0F);

  ASSERT_TRUE(c.collect_once());
  ASSERT_TRUE(ring.take_latest(s));
  EXPECT_FLOAT_EQ(s.runq_p99_us(), 850.0F);  // 失败沿用

  ASSERT_TRUE(c.collect_once());
  ASSERT_TRUE(ring.take_latest(s));
  EXPECT_FLOAT_EQ(s.runq_p99_us(), 2000.0F);
}

TEST(Collector, RunqFailureWithoutHistoryAndDisabled) {
  SampleRing ring;
  RunqResult fail;
  fail.status = RunqResult::kFailed;
  Collector c("w1", std::make_unique<FakeCpu>(1),
              {[] { return 0.5; }, [](int64_t) { return 0.1; }, [=] { return fail; }}, ring,
              std::chrono::milliseconds(1000));
  c.collect_once();
  ASSERT_TRUE(c.collect_once());
  lm::Sample s;
  ASSERT_TRUE(ring.take_latest(s));
  EXPECT_FALSE(s.has_runq_p99_us());  // 没有上次的值可沿用

  RunqResult off;
  off.status = RunqResult::kDisabled;
  SampleRing ring2;
  Collector d("w1", std::make_unique<FakeCpu>(1),
              {[] { return 0.5; }, [](int64_t) { return 0.1; }, [=] { return off; }}, ring2,
              std::chrono::milliseconds(1000));
  d.collect_once();
  ASSERT_TRUE(d.collect_once());
  ASSERT_TRUE(ring2.take_latest(s));
  EXPECT_FALSE(s.has_runq_p50_us());
}

TEST(Collector, FailedReadKeepsLastValue) {
  SampleRing ring;
  int calls = 0;
  Collector c("w1", std::make_unique<FakeCpu>(1),
              {[] { return 0.5; },
               [&calls](int64_t) -> std::optional<double> {
                 return ++calls == 3 ? std::nullopt : std::optional<double>(0.3);
               }},
              ring, std::chrono::milliseconds(1000));
  c.collect_once();
  c.collect_once();
  ASSERT_TRUE(c.collect_once());  // 第 3 次磁盘读取失败，仍出样本
  lm::Sample s;
  ASSERT_TRUE(ring.take_latest(s));
  EXPECT_FLOAT_EQ(s.disk_util(), 0.3F);
}

TEST(Collector, ThreadProducesMonotonicSamples) {
  SampleRing ring;
  Collector c("w1", std::make_unique<FakeCpu>(2), {[] { return 0.5; }, [](int64_t) { return 0.0; }},
              ring, std::chrono::milliseconds(100));
  c.start();
  std::this_thread::sleep_for(std::chrono::milliseconds(350));
  c.stop();
  // 0、100、200、300ms 共 4 次采集，第一次是基线 → 3 条样本（允许调度误差 ±1）
  EXPECT_GE(c.samples_published(), 2U);
  EXPECT_LE(c.samples_published(), 4U);
  lm::Sample s;
  ASSERT_TRUE(ring.take_latest(s));
  EXPECT_EQ(s.seq(), c.samples_published());
}

}  // namespace
