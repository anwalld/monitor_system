#include "collector.h"

#include <spdlog/spdlog.h>

#include "lm/clock.h"

namespace lm::worker {

namespace {
constexpr int kErrorAfterFails = 10;
}

Collector::Collector(std::string node_id, std::unique_ptr<CpuSource> cpu, MetricReaders readers,
                     SampleRing& ring, std::chrono::milliseconds interval)
    : node_id_(std::move(node_id)),
      cpu_(std::move(cpu)),
      readers_(std::move(readers)),
      ring_(ring),
      interval_(interval) {}

Collector::~Collector() { stop(); }

void Collector::start() {
  if (running_.exchange(true)) return;
  thread_ = std::thread([this] { run(); });
}

void Collector::stop() {
  running_.store(false);
  if (thread_.joinable()) thread_.join();
}

void Collector::run() {
  // 按绝对时间对齐周期，避免采集耗时累积成漂移；落后超过一个周期时直接对齐到下一个整周期
  auto next = std::chrono::steady_clock::now();
  while (running_.load(std::memory_order_relaxed)) {
    collect_once();
    next += interval_;
    const auto now = std::chrono::steady_clock::now();
    if (next < now) next = now + interval_;
    // 分段睡眠，stop() 最多等待 100ms
    while (running_.load(std::memory_order_relaxed)) {
      const auto left = next - std::chrono::steady_clock::now();
      if (left <= std::chrono::milliseconds::zero()) break;
      std::this_thread::sleep_for(std::min<std::chrono::steady_clock::duration>(
          left, std::chrono::milliseconds(100)));
    }
  }
}

float Collector::read_with_fallback(const char* name, std::optional<double> v, float& last,
                                    int& fails) {
  if (v) {
    if (fails >= kErrorAfterFails) spdlog::info("{} 读取恢复", name);
    fails = 0;
    last = static_cast<float>(*v);
    return last;
  }
  ++fails;
  if (fails == kErrorAfterFails) {
    spdlog::error("{} 连续 {} 次读取失败，沿用上次的值", name, fails);
  } else if (fails < kErrorAfterFails) {
    spdlog::warn("{} 读取失败，沿用上次的值", name);
  }
  return last;
}

std::optional<RunqStats> Collector::read_runq() {
  if (!readers_.runq) return std::nullopt;
  const RunqResult r = readers_.runq();
  switch (r.status) {
    case RunqResult::kOk:
      if (runq_fails_ >= kErrorAfterFails) spdlog::info("调度延迟读取恢复");
      runq_fails_ = 0;
      last_runq_ = r.stats;
      return last_runq_;
    case RunqResult::kFailed:
      ++runq_fails_;
      if (runq_fails_ == kErrorAfterFails) {
        spdlog::error("调度延迟连续 {} 次读取失败，沿用上次的值", runq_fails_);
      } else if (runq_fails_ < kErrorAfterFails) {
        spdlog::warn("调度延迟读取失败，沿用上次的值");
      }
      return last_runq_;
    case RunqResult::kBaseline:
    case RunqResult::kDisabled:
      break;
  }
  return std::nullopt;
}

bool Collector::collect_once() {
  const int64_t ts = lm::wall_ms();
  const int64_t mono = lm::mono_ms();

  const float mem = read_with_fallback(
      "内存", readers_.mem_avail_ratio ? readers_.mem_avail_ratio() : std::nullopt, last_mem_,
      mem_fails_);
  const float disk = read_with_fallback(
      "磁盘", readers_.disk_util ? readers_.disk_util(mono) : std::nullopt, last_disk_,
      disk_fails_);

  const std::optional<RunqStats> runq = read_runq();

  if (!cpu_->read(snap_)) {
    // CPU 读不到就没有可用的忙碌度，本周期不出样本，也不破坏基线
    ++cpu_fails_;
    if (cpu_fails_ == kErrorAfterFails) {
      spdlog::error("CPU 连续 {} 次读取失败", cpu_fails_);
    } else if (cpu_fails_ < kErrorAfterFails) {
      spdlog::warn("CPU 读取失败");
    }
    return false;
  }
  cpu_fails_ = 0;

  lm::Sample s;
  std::vector<float> core_busy;
  float cpu_busy = 0.0F;
  switch (busy_.update(snap_, core_busy, cpu_busy)) {
    case BusyResult::kBaseline:
      return false;  // 首次只存基线
    case BusyResult::kNoNewData:
      // 读到与上次相同的一份 CPU 数据：沿用上次的忙碌度照常出样本，保证每秒一条
      if (!has_busy_) return false;
      core_busy = last_core_busy_;
      cpu_busy = last_cpu_busy_;
      break;
    case BusyResult::kUpdated:
      last_core_busy_ = core_busy;
      last_cpu_busy_ = cpu_busy;
      has_busy_ = true;
      break;
  }

  s.set_node_id(node_id_);
  s.set_seq(seq_.load(std::memory_order_relaxed) + 1);
  s.set_ts_ms(ts);
  s.set_cpu_busy(cpu_busy);
  for (float b : core_busy) s.add_core_busy(b);
  s.set_mem_avail_ratio(mem);
  s.set_disk_util(disk);
  if (runq) {
    s.set_runq_p50_us(runq->p50_us);
    s.set_runq_p99_us(runq->p99_us);
    s.set_runq_drop_rate(runq->drop_rate);
  }

  ring_.publish(s);
  seq_.fetch_add(1, std::memory_order_relaxed);
  return true;
}

}  // namespace lm::worker
