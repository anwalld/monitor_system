#include <gtest/gtest.h>

#include <thread>

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
    ++n_;
    out.cores.clear();
    for (int i = 0; i < ncpu_; ++i) out.cores.push_back({static_cast<uint32_t>(i), n_ * 50, n_ * 100});
    return true;
  }
  bool fail_next = false;

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
  EXPECT_FALSE(s.has_runq_p99_us());  // M1 不上报调度延迟
  EXPECT_EQ(s.nics_size(), 0);
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
