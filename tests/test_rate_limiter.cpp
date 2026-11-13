#include <gtest/gtest.h>

#include <thread>

#include "lm/log.h"

namespace {

TEST(RateLimiter, AllowsOncePerPeriodAndCountsSuppressed) {
  lm::RateLimiter rl(std::chrono::milliseconds(200));
  uint64_t s = 99;
  EXPECT_TRUE(rl.allow(&s));
  EXPECT_EQ(s, 0U);
  EXPECT_FALSE(rl.allow());
  EXPECT_FALSE(rl.allow());
  std::this_thread::sleep_for(std::chrono::milliseconds(250));
  EXPECT_TRUE(rl.allow(&s));
  EXPECT_EQ(s, 2U);
}

}  // namespace
