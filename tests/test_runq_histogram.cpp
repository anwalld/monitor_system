#include <gtest/gtest.h>

#include "collector/runq_histogram.h"
#include "runqlat.h"

using namespace lm::worker;

namespace {

// 参考实现：用 64 位整数直接按定义求桶号
unsigned ref_bucket(uint64_t v) {
  if (v < 8) return static_cast<unsigned>(v);
  unsigned k = 63;
  while (!((v >> k) & 1U)) --k;
  const unsigned b = (k - 2) * 8 + static_cast<unsigned>((v >> (k - 3)) & 7U);
  return b < RUNQ_BUCKETS ? b : RUNQ_BUCKETS - 1;
}

TEST(RunqBucket, SpecBoundaries) {
  EXPECT_EQ(runq_bucket_of(0), 0U);
  EXPECT_EQ(runq_bucket_of(7), 7U);
  EXPECT_EQ(runq_bucket_of(8), 8U);
  EXPECT_EQ(runq_bucket_of(15), 15U);
  EXPECT_EQ(runq_bucket_of(16), 16U);
  EXPECT_EQ(runq_bucket_of(100), 36U);
  EXPECT_EQ(runq_bucket_of(1000), 63U);
  EXPECT_EQ(runq_bucket_of(1ULL << 30), 207U);
  EXPECT_EQ(runq_bucket_of(~0ULL), 207U);
}

TEST(RunqBucket, MatchesReferenceAndBounds) {
  for (uint64_t v = 0; v <= (1U << 20); ++v) {
    const unsigned b = runq_bucket_of(v);
    ASSERT_EQ(b, ref_bucket(v)) << "v=" << v;
    // 每个值都落在自己桶的区间内（未截断的桶）
    if (b < RUNQ_BUCKETS - 1) {
      ASSERT_GE(v, runq_bucket_low(b)) << "v=" << v;
      ASSERT_LT(v, runq_bucket_low(b) + runq_bucket_width(b)) << "v=" << v;
    }
  }
  // 相邻桶首尾相接
  for (unsigned b = 0; b + 1 < RUNQ_BUCKETS; ++b) {
    EXPECT_EQ(runq_bucket_low(b) + runq_bucket_width(b), runq_bucket_low(b + 1)) << "b=" << b;
  }
}

TEST(RunqPercentile, InterpolatesWithinBucket) {
  std::array<uint64_t, RUNQ_BUCKETS> d{};
  d[16] = 100;  // [16, 18)
  EXPECT_DOUBLE_EQ(runq_percentile(d, 0.50), 17.0);
  EXPECT_NEAR(runq_percentile(d, 0.99), 17.98, 1e-9);
}

TEST(RunqPercentile, AcrossBucketsAndEmpty) {
  std::array<uint64_t, RUNQ_BUCKETS> d{};
  EXPECT_EQ(runq_percentile(d, 0.5), 0.0);
  d[2] = 50;   // [2, 3)
  d[63] = 50;  // [960, 1024)
  EXPECT_DOUBLE_EQ(runq_percentile(d, 0.5), 3.0);    // 秩 50 正好是第 2 桶末尾
  EXPECT_NEAR(runq_percentile(d, 0.99), 960 + 64 * 0.98, 1e-9);
}

TEST(RunqDropRate, Basic) {
  EXPECT_DOUBLE_EQ(runq_drop_rate(10, 990), 0.01);
  EXPECT_DOUBLE_EQ(runq_drop_rate(0, 0), 0.0);
  EXPECT_DOUBLE_EQ(runq_drop_rate(5, 0), 1.0);
}

TEST(RunqWindow, BaselineThenDelta) {
  RunqWindow w;
  RunqCumulative c;
  c.buckets[8] = 1000;
  c.drops = 3;
  EXPECT_FALSE(w.update(c).has_value());
  c.buckets[8] = 1010;
  auto s = w.update(c);
  ASSERT_TRUE(s.has_value());
  EXPECT_EQ(s->samples, 10U);
  EXPECT_EQ(s->drops, 0U);
  EXPECT_FLOAT_EQ(s->p50_us, 8.5F);  // [8, 9) 中点
  EXPECT_EQ(w.last_delta()[8], 10U);
}

TEST(RunqWindow, IdlePeriodIsAllZero) {
  RunqWindow w;
  RunqCumulative c;
  c.buckets[20] = 7;
  w.update(c);
  auto s = w.update(c);
  ASSERT_TRUE(s.has_value());
  EXPECT_EQ(s->samples, 0U);
  EXPECT_EQ(s->p50_us, 0.0F);
  EXPECT_EQ(s->p99_us, 0.0F);
  EXPECT_EQ(s->drop_rate, 0.0F);
}

TEST(RunqWindow, CounterGoingBackRebaselines) {
  RunqWindow w;
  RunqCumulative c;
  c.buckets[5] = 100;
  w.update(c);
  c.buckets[5] = 10;  // 倒退
  EXPECT_FALSE(w.update(c).has_value());
  c.buckets[5] = 30;
  auto s = w.update(c);
  ASSERT_TRUE(s.has_value());
  EXPECT_EQ(s->samples, 20U);
}

TEST(RunqWindow, DropRateFromDeltas) {
  RunqWindow w;
  RunqCumulative c;
  w.update(c);
  c.buckets[10] = 990;
  c.drops = 10;
  auto s = w.update(c);
  ASSERT_TRUE(s.has_value());
  EXPECT_FLOAT_EQ(s->drop_rate, 0.01F);
}

}  // namespace
