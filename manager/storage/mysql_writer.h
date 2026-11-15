#pragma once

// MySQL 写线程：消费 MysqlQueue，写 node、node_metrics_5m、node_event、task 四张表。
// 每秒刷新一次：已结束的 5 分钟桶、1 秒内合并的任务变化、缓冲的事件。写失败只记日志（限频）。

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <variant>
#include <vector>

#include "events.h"
#include "lm/log.h"
#include "metrics_agg.h"
#include "mysql_client.h"
#include "storage_queue.h"
#include "task_rows.h"

namespace lm::manager {

struct NodeRow {
  std::string node_id;
  std::string addr;
  uint32_t ncpu = 0;
  int64_t first_seen_ms = 0;
};

using MysqlItem = std::variant<MetricPoint, NodeRow, NodeEvent, TaskChange>;
using MysqlQueue = BoundedQueue<MysqlItem>;

class MysqlWriter {
 public:
  struct Options {
    MysqlOptions db;
    std::chrono::milliseconds flush_interval{1000};
    int64_t bucket_ms = 300 * 1000;
    int64_t grace_ms = 5000;
    std::size_t max_pending_rows = 50000;  // 事件、节点缓冲上限（任务由 TaskMerger 自己限）
  };

  MysqlWriter(MysqlQueue& queue, Options opt);
  ~MysqlWriter();

  void start();
  void stop();  // 写出未结束的桶与缓冲后退出

  // 以下供测试直接驱动。
  void consume(std::vector<MysqlItem> items);
  void flush(int64_t now_wall_ms, int64_t now_mono_ms, bool all);

  uint64_t rows_written() const { return rows_.load(); }
  uint64_t errors() const { return errors_.load(); }
  uint64_t dropped() const { return dropped_.load(); }

  // SQL 生成（测试用）。
  std::string sql_metrics(const std::vector<MetricsRow>& rows) const;
  std::string sql_events(const std::vector<NodeEvent>& evs) const;
  std::string sql_nodes(const std::vector<NodeRow>& nodes) const;
  std::string sql_task_insert(const std::vector<TaskRow>& rows) const;
  std::string sql_task_update(const TaskRow& r) const;

 private:
  void run();
  bool exec(const char* what, const std::string& sql, std::size_t rows);

  MysqlQueue& queue_;
  const Options opt_;
  MysqlClient db_;
  MetricsAggregator agg_;
  TaskMerger tasks_;
  std::vector<NodeEvent> events_;
  std::vector<NodeRow> nodes_;
  std::vector<MetricsRow> retry_buckets_;  // 写库时连接断开的已结束桶，重连后先写
  lm::RateLimiter err_log_{std::chrono::seconds(10)};
  std::atomic<uint64_t> rows_{0};
  std::atomic<uint64_t> errors_{0};
  std::atomic<uint64_t> dropped_{0};
  uint64_t local_dropped_ = 0;  // 只在 MySQL 线程里改
  std::atomic<bool> running_{false};
  std::thread thread_;
};

// 毫秒墙上时间 → MySQL DATETIME(3) 字面量（UTC），如 '2026-11-14 01:05:00.123'。
std::string mysql_datetime(int64_t wall_ms, bool millis = true);

}  // namespace lm::manager
