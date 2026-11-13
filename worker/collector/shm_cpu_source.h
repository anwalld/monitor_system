#pragma once

// 从 cpustat 内核模块的共享页读取 CPU 读数（M2）。
// 每核 idle = idle_us + iowait_us，total = stamp_us（真实时间戳，微秒），忙碌度仍由 CpuBusyCalculator 计算。

#include <cstdint>
#include <string>

#include "cpu_source.h"
#include "cpustat_uapi.h"

namespace lm::worker {

class ShmCpuSource : public CpuSource {
 public:
  // 打开并只读映射 path；失败抛 std::runtime_error，消息包含路径和 errno 描述。
  explicit ShmCpuSource(const std::string& path = CPUSTAT_DEV_PATH);
  ~ShmCpuSource() override;
  ShmCpuSource(const ShmCpuSource&) = delete;
  ShmCpuSource& operator=(const ShmCpuSource&) = delete;

  bool read(CpuSnapshot& out) override;

  // 共享页当前的刷新周期（微秒）。
  uint32_t interval_us() const;
  // 通过 ioctl 修改刷新周期；失败返回 false 并保留 errno。非 Linux 平台始终失败。
  bool set_interval_us(uint32_t us);

  int fd() const { return fd_; }
  const std::string& path() const { return path_; }

 private:
  std::string path_;
  int fd_ = -1;
  const cpustat_shm* shm_ = nullptr;
  cpustat_core buf_[CPUSTAT_MAX_CPU];
};

}  // namespace lm::worker
