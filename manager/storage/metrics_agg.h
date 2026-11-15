#pragma once

// 5 分钟聚合：按节点、按墙上时间对齐的桶累计样本摘要，桶结束（加宽限）后取出写 node_metrics_5m。
// 只在 MySQL 线程里使用，不加锁。

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace lm::manager {

struct MetricPoint {
  std::string node_id;
  int64_t ts_ms = 0;  // 接收时的墙上时间
  double cpu = 0;
  std::optional<double> runq_p99_us;
  double mem = 0;
  std::optional<double> rtt_ms;       // 各网卡平均
  std::optional<double> timeout_rate;
  std::optional<double> retrans_rate;
  double score = 0;
};

struct MetricsRow {
  std::string node_id;
  int64_t bucket_start_ms = 0;
  double cpu_avg = 0;
  double cpu_max = 0;
  std::optional<double> runq_p99_max_us;
  double mem_avail_min = 0;
  std::optional<double> net_rtt_avg_ms;
  std::optional<double> net_timeout_rate;
  std::optional<double> net_retrans_rate;
  double score_avg = 0;
  int samples = 0;
};

class MetricsAggregator {
 public:
  explicit MetricsAggregator(int64_t bucket_ms = 300 * 1000, int64_t grace_ms = 5000)
      : bucket_ms_(bucket_ms), grace_ms_(grace_ms) {}

  void add(const MetricPoint& p);
  // 取出所有“结束时刻 + 宽限 ≤ now”的桶。
  std::vector<MetricsRow> take_finished(int64_t now_ms);
  // 取出全部（退出时）。
  std::vector<MetricsRow> take_all();
  int64_t bucket_of(int64_t ts_ms) const { return ts_ms - ((ts_ms % bucket_ms_) + bucket_ms_) % bucket_ms_; }
  std::size_t open_buckets() const { return acc_.size(); }

 private:
  struct Acc {
    double cpu_sum = 0, cpu_max = 0;
    std::optional<double> runq_max;
    double mem_min = 0;
    double rtt_sum = 0, to_sum = 0, rt_sum = 0;
    int net_n = 0;
    double score_sum = 0;
    int n = 0;
  };
  static MetricsRow finish(const std::pair<std::string, int64_t>& key, const Acc& a);

  const int64_t bucket_ms_;
  const int64_t grace_ms_;
  std::map<std::pair<std::string, int64_t>, Acc> acc_;  // (node, bucket_start) → 累计
};

}  // namespace lm::manager
