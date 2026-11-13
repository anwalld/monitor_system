#include <algorithm>

#include "cpu_source.h"

namespace lm::worker {

namespace {

bool same_cpu_set(const CpuSnapshot& a, const CpuSnapshot& b) {
  if (a.cores.size() != b.cores.size()) return false;
  for (std::size_t i = 0; i < a.cores.size(); ++i) {
    if (a.cores[i].cpu != b.cores[i].cpu) return false;
  }
  return true;
}

}  // namespace

bool CpuBusyCalculator::update(const CpuSnapshot& snap, std::vector<float>& core_busy,
                               float& cpu_busy) {
  if (snap.cores.empty()) return false;
  if (!has_prev_ || !same_cpu_set(prev_, snap)) {
    prev_ = snap;
    has_prev_ = true;
    return false;
  }

  core_busy.assign(snap.cores.size(), 0.0F);
  double sum = 0.0;
  for (std::size_t i = 0; i < snap.cores.size(); ++i) {
    const CoreTimes& cur = snap.cores[i];
    const CoreTimes& old = prev_.cores[i];
    // 计数器倒退（来源重置）或时间没走，都按 0 处理，不产生负值或除零
    const uint64_t dt = cur.total > old.total ? cur.total - old.total : 0;
    const uint64_t didle = cur.idle > old.idle ? cur.idle - old.idle : 0;
    double busy = 0.0;
    if (dt > 0) {
      busy = static_cast<double>(dt > didle ? dt - didle : 0) / static_cast<double>(dt);
    }
    busy = std::clamp(busy, 0.0, 1.0);
    core_busy[i] = static_cast<float>(busy);
    sum += busy;
  }
  cpu_busy = static_cast<float>(sum / static_cast<double>(snap.cores.size()));
  prev_ = snap;
  return true;
}

}  // namespace lm::worker
