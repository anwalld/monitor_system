#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <random>
#include <thread>

#include "lm/ring.h"

namespace {

TEST(Ring, EmptyReturnsFalse) {
  lm::Ring<int, 4> r;
  int v = 0;
  EXPECT_FALSE(r.take_latest(v));
}

TEST(Ring, TakesLatestAndOnlyOnce) {
  lm::Ring<int, 4> r;
  r.publish(1);
  r.publish(2);
  r.publish(3);
  int v = 0;
  uint64_t seq = 0;
  ASSERT_TRUE(r.take_latest(v, &seq));
  EXPECT_EQ(v, 3);
  EXPECT_EQ(seq, 3U);
  // 1、2 比已取的 3 旧，不再返回
  EXPECT_FALSE(r.take_latest(v));
  r.publish(4);
  ASSERT_TRUE(r.take_latest(v, &seq));
  EXPECT_EQ(v, 4);
}

TEST(Ring, OverwritesOldestWhenFullAndReaderSkipsBacklog) {
  lm::Ring<int, 4> r;
  for (int i = 1; i <= 10; ++i) r.publish(i);
  int v = 0;
  ASSERT_TRUE(r.take_latest(v));
  EXPECT_EQ(v, 10);  // 积压的 7..9 被跳过
  EXPECT_FALSE(r.take_latest(v));
}

TEST(Ring, ReadingSlotIsNotOverwritten) {
  // 消费者占着一个槽时，生产者写满一圈也不会动它：用 N=2 时最多只剩 1 个可写槽
  lm::Ring<int, 2> r;
  r.publish(1);
  r.publish(2);
  r.publish(3);  // 覆盖 1
  int v = 0;
  ASSERT_TRUE(r.take_latest(v));
  EXPECT_EQ(v, 3);
  EXPECT_EQ(r.publish(4), 4U);
  EXPECT_EQ(r.publish(5), 5U);
  ASSERT_TRUE(r.take_latest(v));
  EXPECT_EQ(v, 5);
}

// 一半写入一半为另一个值的结构，用来检测撕裂
struct Pair {
  uint64_t a = 0;
  uint64_t pad[6] = {};
  uint64_t b = 0;
};

TEST(Ring, ConcurrentNoTearingAndMonotonicSeq) {
  lm::Ring<Pair, 8> r;
  std::atomic<bool> stop{false};
  std::thread producer([&] {
    uint64_t i = 0;
    while (!stop.load()) {
      ++i;
      Pair p;
      p.a = i;
      for (auto& x : p.pad) x = i;
      p.b = i;
      r.publish(p);
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });

  std::mt19937 rng(42);
  std::uniform_int_distribution<int> delay(0, 3);
  uint64_t last_seq = 0, reads = 0, torn = 0, backwards = 0;
  const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (std::chrono::steady_clock::now() < end) {
    Pair p;
    uint64_t seq = 0;
    if (r.take_latest(p, &seq)) {
      ++reads;
      bool ok = p.a == p.b;
      for (auto x : p.pad) ok = ok && x == p.a;
      if (!ok) ++torn;
      if (seq < last_seq) ++backwards;
      last_seq = seq;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(delay(rng)));
  }
  stop.store(true);
  producer.join();
  EXPECT_GT(reads, 1000U);
  EXPECT_EQ(torn, 0U);
  EXPECT_EQ(backwards, 0U);
}

}  // namespace
