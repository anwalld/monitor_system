#include "interval_guard.h"

#include <cerrno>
#include <cstring>

#include <spdlog/spdlog.h>

#include "shm_cpu_source.h"

namespace lm::worker {

IntervalGuard::IntervalGuard(CpuSource* source, uint32_t interval_us) {
  auto* shm = dynamic_cast<ShmCpuSource*>(source);
  if (shm == nullptr) return;  // /proc 来源没有刷新周期可调
  const uint32_t previous = shm->interval_us();
  if (!shm->set_interval_us(interval_us)) {
    spdlog::warn("设置 cpustat 刷新周期为 {}us 失败：{}", interval_us, std::strerror(errno));
    return;
  }
  shm_ = shm;
  previous_us_ = previous;
  spdlog::info("cpustat 刷新周期 {}us -> {}us", previous, interval_us);
}

IntervalGuard::~IntervalGuard() {
  if (shm_ == nullptr) return;
  if (!shm_->set_interval_us(previous_us_)) {
    spdlog::warn("恢复 cpustat 刷新周期为 {}us 失败：{}", previous_us_, std::strerror(errno));
    return;
  }
  spdlog::info("cpustat 刷新周期恢复为 {}us", previous_us_);
}

}  // namespace lm::worker
