#pragma once

// 日志初始化与限频。所有组件统一用 spdlog 默认 logger。

#include <atomic>
#include <chrono>
#include <cstdint>
#include <string>

#include <spdlog/spdlog.h>

namespace lm {

// 设置默认 logger 的格式与级别；level 取 trace/debug/info/warn/error/critical/off，非法值按 info。
void init_logging(const std::string& name, const std::string& level);

// 限频器：每个周期最多放行一次，并报告周期内被抑制的次数。线程安全。
//   static lm::RateLimiter rl(std::chrono::seconds(10));
//   uint64_t suppressed;
//   if (rl.allow(&suppressed)) spdlog::warn("Report failed ({} suppressed)", suppressed);
class RateLimiter {
 public:
  explicit RateLimiter(std::chrono::steady_clock::duration period) : period_(period) {}

  bool allow(uint64_t* suppressed = nullptr);

 private:
  const std::chrono::steady_clock::duration period_;
  std::atomic<int64_t> next_ns_{0};
  std::atomic<uint64_t> suppressed_{0};
};

}  // namespace lm
