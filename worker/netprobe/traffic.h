#pragma once

// 网卡流量：sysfs statistics/{rx,tx}_bytes 常开 fd，每周期 pread；RateCounter 把累计字节换算成比特每秒。

#include <cstdint>
#include <optional>
#include <string>

namespace lm::worker {

class RateCounter {
 public:
  // 第一次返回 0；计数倒退时返回 0 并重建基线；否则返回 Δ字节 × 8 / Δ秒。
  uint64_t update(uint64_t bytes, int64_t now_ns);

 private:
  bool has_prev_ = false;
  uint64_t prev_bytes_ = 0;
  int64_t prev_ns_ = 0;
};

// 常开一个 sysfs 计数文件，每次 pread 读出十进制值。
class SysfsCounter {
 public:
  explicit SysfsCounter(const std::string& path);
  ~SysfsCounter();
  SysfsCounter(const SysfsCounter&) = delete;
  SysfsCounter& operator=(const SysfsCounter&) = delete;

  bool ok() const { return fd_ >= 0; }
  std::optional<uint64_t> read() const;

 private:
  int fd_ = -1;
};

}  // namespace lm::worker
