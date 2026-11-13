#include <gtest/gtest.h>

#include "storage/storage_queue.h"

using lm::manager::BoundedQueue;

namespace {

TEST(BoundedQueue, DropsOldestWhenFull) {
  BoundedQueue<int> q(3);
  for (int i = 1; i <= 5; ++i) q.push(i);
  EXPECT_EQ(q.dropped(), 2U);
  auto b = q.pop_batch(10, std::chrono::milliseconds(0));
  EXPECT_EQ(b, (std::vector<int>{3, 4, 5}));
}

TEST(BoundedQueue, PopBatchWaitsUntilFullOrTimeout) {
  BoundedQueue<int> q(10);
  q.push(1);
  const auto t0 = std::chrono::steady_clock::now();
  auto b = q.pop_batch(5, std::chrono::milliseconds(50));
  EXPECT_GE(std::chrono::steady_clock::now() - t0, std::chrono::milliseconds(45));
  EXPECT_EQ(b, (std::vector<int>{1}));
  q.close();
  EXPECT_TRUE(q.pop_batch(5, std::chrono::seconds(5)).empty());  // close 后立即返回
}

}  // namespace
