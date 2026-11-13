#pragma once

// 采集线程：按固定周期采一次 CPU、内存、磁盘，组装 Sample 写入环形缓冲。
// 调度延迟（M3）与网卡（M4）字段暂不填。

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include "cpu_source.h"
#include "lm/ring.h"
#include "monitor.pb.h"

namespace lm::worker {

using SampleRing = lm::Ring<lm::Sample, 8>;

// 内存与磁盘读取接口，便于测试替换。
struct MetricReaders {
  std::function<std::optional<double>()> mem_avail_ratio;
  std::function<std::optional<double>(int64_t now_mono_ms)> disk_util;
};

class Collector {
 public:
  Collector(std::string node_id, std::unique_ptr<CpuSource> cpu, MetricReaders readers,
            SampleRing& ring, std::chrono::milliseconds interval);
  ~Collector();

  void start();
  void stop();

  // 采集一次；生成了样本返回 true（第一次只建立基线返回 false）。供测试与主循环使用。
  bool collect_once();

  uint64_t samples_published() const { return seq_.load(std::memory_order_relaxed); }

 private:
  void run();
  float read_with_fallback(const char* name, std::optional<double> v, float& last, int& fails);

  const std::string node_id_;
  std::unique_ptr<CpuSource> cpu_;
  MetricReaders readers_;
  SampleRing& ring_;
  const std::chrono::milliseconds interval_;

  CpuBusyCalculator busy_;
  CpuSnapshot snap_;
  float last_mem_ = 0.0F;
  float last_disk_ = 0.0F;
  int mem_fails_ = 0;
  int disk_fails_ = 0;
  int cpu_fails_ = 0;
  std::atomic<uint64_t> seq_{0};

  std::atomic<bool> running_{false};
  std::thread thread_;
};

}  // namespace lm::worker
