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

enum class BusyResult {
  kBaseline,   // 第一次调用或 CPU 集合变化：只存基线，没有忙碌度
  kNoNewData,  // 与上次是同一份数据（所有核 total 都没变）：不更新基线，调用方沿用上次的值
  kUpdated,    // 已算出新的忙碌度
};

// 由相邻两次快照计算每核与全机忙碌度。
// 共享页按固定周期刷新，与采集周期相位无关，相邻两次采集可能读到同一次刷新，此时返回 kNoNewData，
// 下一次按更长的时间差计算，平均值仍正确。
class CpuBusyCalculator {
 public:
  BusyResult update(const CpuSnapshot& snap, std::vector<float>& core_busy, float& cpu_busy);
  void reset() { has_prev_ = false; }

 private:
  bool has_prev_ = false;
  CpuSnapshot prev_;
};

}  // namespace lm::worker
