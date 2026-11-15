#pragma once

// task 表的写入：在途账的每一步生成一条 TaskChange，MySQL 线程按 task_id 合并 1 秒内的变化再批量写。

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace lm::manager {

enum class TaskKind { kAllocated, kSent, kReceived, kDone, kFailed, kTimeout, kVoid };

const char* task_state_name(TaskKind k);  // allocated / sent / received / done / failed / timeout / void
int task_rank(TaskKind k);                // allocated 0 < sent 1 < received 2 < 终态 3
bool task_terminal(TaskKind k);

struct TaskChange {
  std::string task_id;
  std::string node_id;  // 只有分配时有
  uint64_t input_bytes = 0;
  TaskKind kind = TaskKind::kAllocated;
  int64_t ts_ms = 0;  // 墙上时间
};

// 在途账的变化交给 MySQL 队列（main 里接到 MysqlQueue::push；未设置时不生成）。
using TaskSink = std::function<void(TaskChange)>;

struct TaskRow {
  std::string task_id;
  bool has_alloc = false;  // 本批包含分配信息：INSERT；否则 UPDATE
  std::string node_id;
  uint64_t input_bytes = 0;
  TaskKind state = TaskKind::kAllocated;
  std::optional<int64_t> t_alloc, t_sent, t_recv, t_done;
};

// 新状态能否覆盖旧状态：等级更高，或迟到的 done/failed 覆盖 timeout。
bool task_state_overrides(TaskKind old_state, TaskKind new_state);

class TaskMerger {
 public:
  explicit TaskMerger(std::size_t cap = 50000) : cap_(cap) {}
  void add(const TaskChange& c);
  std::vector<TaskRow> take();  // 取出全部（插入顺序）
  // 写库时连接断开：把取出的行放回去（与此后新到的变化按同样规则合并）。
  void restore(std::vector<TaskRow> rows);
  std::size_t size() const { return rows_.size(); }
  uint64_t dropped() const { return dropped_; }

 private:
  std::size_t cap_;
  std::unordered_map<std::string, std::size_t> index_;
  std::vector<TaskRow> rows_;
  uint64_t dropped_ = 0;
};

}  // namespace lm::manager
