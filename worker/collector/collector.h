#pragma once

// 采集线程：按固定周期采一次 CPU、内存、磁盘，组装 Sample 写入环形缓冲。
// 调度延迟由可选的 runq 读取函数提供（M3）；网卡（M4）字段暂不填。

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "cpu_source.h"
#include "runq_histogram.h"
#include "lm/ring.h"
#include "monitor.pb.h"

namespace lm::worker {

using SampleRing = lm::Ring<lm::Sample, 8>;

// 调度延迟读取结果。
struct RunqResult {
  enum Status { kOk, kBaseline, kFailed, kDisabled };
  Status status = kDisabled;
  RunqStats stats;
};

// 各项指标的读取接口，便于测试替换。runq 为空等同 kDisabled（不设置调度延迟字段）。
struct MetricReaders {
  std::function<std::optional<double>()> mem_avail_ratio;
  std::function<std::optional<double>(int64_t now_mono_ms)> disk_util;
  std::function<RunqResult()> runq;
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
  std::optional<RunqStats> read_runq();
  float read_with_fallback(const char* name, std::optional<double> v, float& last, int& fails);

  const std::string node_id_;
  std::unique_ptr<CpuSource> cpu_;
  MetricReaders readers_;
  SampleRing& ring_;
  const std::chrono::milliseconds interval_;

  CpuBusyCalculator busy_;
  CpuSnapshot snap_;
  std::vector<float> last_core_busy_;
  float last_cpu_busy_ = 0.0F;
  bool has_busy_ = false;
  std::optional<RunqStats> last_runq_;
  int runq_fails_ = 0;
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
