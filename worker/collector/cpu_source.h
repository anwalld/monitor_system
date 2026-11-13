#pragma once

// CPU 读数来源。M1 用 /proc/stat，M2 换成 cpustat 内核模块共享页；上层只依赖本接口。
// 每核给出两个单调递增的累计量：空闲（idle + iowait）与总时间，单位由来源决定但两者一致
// （/proc/stat 为 jiffies，共享页为微秒）。忙碌度只用它们的差值计算。

#include <cstdint>
#include <vector>

namespace lm::worker {

struct CoreTimes {
  uint32_t cpu = 0;     // CPU 编号
  uint64_t idle = 0;    // 累计空闲（idle + iowait）
  uint64_t total = 0;   // 累计总时间
};

struct CpuSnapshot {
  std::vector<CoreTimes> cores;  // 按 CPU 编号升序
};

class CpuSource {
 public:
  virtual ~CpuSource() = default;
  // 读取一次快照；失败返回 false。
  virtual bool read(CpuSnapshot& out) = 0;
};

// 由相邻两次快照计算每核与全机忙碌度。第一次调用、或 CPU 集合变化时只存基线返回 false。
class CpuBusyCalculator {
 public:
  bool update(const CpuSnapshot& snap, std::vector<float>& core_busy, float& cpu_busy);
  void reset() { has_prev_ = false; }

 private:
  bool has_prev_ = false;
  CpuSnapshot prev_;
};

}  // namespace lm::worker
