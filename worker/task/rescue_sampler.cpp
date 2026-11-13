#include "rescue_sampler.h"

#include <chrono>

#include <spdlog/spdlog.h>

namespace lm::worker {

void RescueSampler::begin() {
  if (started_) return;
  started_ = true;
  has_busy_ = false;
  calc_.reset();
  try {
    source_ = make_source_ ? make_source_() : nullptr;
  } catch (const std::exception& e) {
    spdlog::warn("急救：无法打开 CPU 来源（{}），回应只带最新样本", e.what());
    source_.reset();
  }
  if (!source_) return;
  guard_ = std::make_unique<IntervalGuard>(source_.get(), interval_us_);
  if (source_->read(snap_)) calc_.update(snap_, core_busy_, cpu_busy_);  // 建基线
}

void RescueSampler::end() {
  guard_.reset();  // 先恢复周期
  source_.reset();
  started_ = false;
}

lm::Sample RescueSampler::sample() {
  lm::Sample s = latest_ ? latest_() : lm::Sample{};
  if (source_ && source_->read(snap_)) {
    if (calc_.update(snap_, core_busy_, cpu_busy_) == BusyResult::kUpdated) has_busy_ = true;
  }
  if (has_busy_) {
    s.set_cpu_busy(cpu_busy_);
    s.clear_core_busy();
    for (float v : core_busy_) s.add_core_busy(v);
  }
  s.set_ts_ms(std::chrono::duration_cast<std::chrono::milliseconds>(
                  std::chrono::system_clock::now().time_since_epoch())
                  .count());
  return s;
}

}  // namespace lm::worker
