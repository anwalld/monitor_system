#include <gtest/gtest.h>

#include "lm/config.h"

namespace {

TEST(Config, ReadsNestedAndFallback) {
  auto c = lm::Config::load_string("listen: 0.0.0.0:50051\nredis:\n  uri: tcp://r:6379\n  ts_retention_s: 60\n");
  EXPECT_EQ(c.get<std::string>("listen", ""), "0.0.0.0:50051");
  EXPECT_EQ(c.get<std::string>("redis.uri", ""), "tcp://r:6379");
  EXPECT_EQ(c.get<int>("redis.ts_retention_s", 0), 60);
  EXPECT_EQ(c.get<int>("redis.missing", 7), 7);
  EXPECT_EQ(c.get<int>("nope.deeper", 9), 9);
  EXPECT_TRUE(c.has("redis.uri"));
  EXPECT_FALSE(c.has("redis.nope"));
}

TEST(Config, LookupDoesNotMutateTree) {
  auto c = lm::Config::load_string("a:\n  b: 1\n");
  (void)c.get<int>("a.x", 0);
  (void)c.get<int>("a.b", 0);
  EXPECT_EQ(c.get<int>("a.b", 0), 1);
  EXPECT_FALSE(c.has("a.x"));
}

TEST(Config, WrongTypeNamesTheKey) {
  auto c = lm::Config::load_string("collect_interval_ms: abc\n");
  try {
    (void)c.get<int>("collect_interval_ms", 1000);
    FAIL() << "expected ConfigError";
  } catch (const lm::ConfigError& e) {
    EXPECT_NE(std::string(e.what()).find("collect_interval_ms"), std::string::npos);
  }
}

TEST(Config, RequireMissing) {
  auto c = lm::Config::load_string("x: 1\n");
  EXPECT_THROW((void)c.require<std::string>("manager_addr"), lm::ConfigError);
}

TEST(Config, MissingFile) {
  EXPECT_THROW(lm::Config::load_file("/nonexistent/lm.yaml"), lm::ConfigError);
}

}  // namespace
