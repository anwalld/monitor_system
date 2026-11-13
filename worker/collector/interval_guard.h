#pragma once

// 刷新周期守卫：作用域内把 cpustat 刷新周期改为指定值（急救用 1000 微秒），离开作用域时恢复进入前的周期。
// 来源不是共享页时什么也不做；设置失败只记警告，不影响采集。M6 急救流程使用。
// Worker 进程崩溃时由内核模块在打开文件释放时恢复默认周期兜底。

#include <cstdint>

#include "cpu_source.h"

namespace lm::worker {

class ShmCpuSource;

class IntervalGuard {
 public:
  IntervalGuard(CpuSource* source, uint32_t interval_us);
  ~IntervalGuard();
  IntervalGuard(const IntervalGuard&) = delete;
  IntervalGuard& operator=(const IntervalGuard&) = delete;

  // 是否真的改了周期（析构时才会恢复）。
  bool active() const { return shm_ != nullptr; }

 private:
  ShmCpuSource* shm_ = nullptr;
  uint32_t previous_us_ = 0;
};

}  // namespace lm::worker
