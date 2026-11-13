#pragma once

#include <chrono>
#include <cstdint>

namespace lm {

// Unix 毫秒（墙上时间），用于样本时间戳与 Redis 中的时间。
inline int64_t wall_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// 单调时钟毫秒，用于超时与新鲜度判断。
inline int64_t mono_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace lm
