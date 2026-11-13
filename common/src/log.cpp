#include "lm/log.h"

#include <spdlog/sinks/stdout_color_sinks.h>

namespace lm {

void init_logging(const std::string& name, const std::string& level) {
  // 日志写 stderr：标准输出留给 worker --dry-run 等输出数据的场景（systemd 下两者都进 journal）
  auto logger = spdlog::stderr_color_mt(name);
  spdlog::set_default_logger(logger);
  spdlog::set_pattern("%Y-%m-%d %H:%M:%S.%e [%n] [%^%l%$] [%t] %v");
  auto lvl = spdlog::level::from_str(level);
  if (lvl == spdlog::level::off && level != "off") lvl = spdlog::level::info;
  spdlog::set_level(lvl);
  spdlog::flush_on(spdlog::level::warn);
}

bool RateLimiter::allow(uint64_t* suppressed) {
  const int64_t now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::steady_clock::now().time_since_epoch())
                          .count();
  int64_t next = next_ns_.load(std::memory_order_relaxed);
  const int64_t period = std::chrono::duration_cast<std::chrono::nanoseconds>(period_).count();
  while (now >= next) {
    if (next_ns_.compare_exchange_weak(next, now + period, std::memory_order_relaxed)) {
      const uint64_t s = suppressed_.exchange(0, std::memory_order_relaxed);
      if (suppressed != nullptr) *suppressed = s;
      return true;
    }
  }
  suppressed_.fetch_add(1, std::memory_order_relaxed);
  return false;
}

}  // namespace lm
