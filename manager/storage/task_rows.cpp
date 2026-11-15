#include "task_rows.h"

namespace lm::manager {

const char* task_state_name(TaskKind k) {
  switch (k) {
    case TaskKind::kAllocated: return "allocated";
    case TaskKind::kSent: return "sent";
    case TaskKind::kReceived: return "received";
    case TaskKind::kDone: return "done";
    case TaskKind::kFailed: return "failed";
    case TaskKind::kTimeout: return "timeout";
    case TaskKind::kVoid: return "void";
  }
  return "?";
}

bool task_terminal(TaskKind k) { return task_rank(k) == 3; }

int task_rank(TaskKind k) {
  switch (k) {
    case TaskKind::kAllocated: return 0;
    case TaskKind::kSent: return 1;
    case TaskKind::kReceived: return 2;
    default: return 3;
  }
}

bool task_state_overrides(TaskKind old_state, TaskKind new_state) {
  if (task_rank(new_state) > task_rank(old_state)) return true;
  return old_state == TaskKind::kTimeout &&
         (new_state == TaskKind::kDone || new_state == TaskKind::kFailed);
}

void TaskMerger::add(const TaskChange& c) {
  auto it = index_.find(c.task_id);
  if (it == index_.end()) {
    if (rows_.size() >= cap_) {
      ++dropped_;  // 超过上限：直接丢弃新任务（1 秒内 5 万个任务远超规模）
      return;
    }
    TaskRow r;
    r.task_id = c.task_id;
    r.state = c.kind;
    it = index_.emplace(c.task_id, rows_.size()).first;
    rows_.push_back(std::move(r));
  } else if (task_state_overrides(rows_[it->second].state, c.kind)) {
    rows_[it->second].state = c.kind;
  }
  TaskRow& r = rows_[it->second];
  auto first = [&](std::optional<int64_t>& slot) {
    if (!slot) slot = c.ts_ms;
  };
  switch (c.kind) {
    case TaskKind::kAllocated:
      r.has_alloc = true;
      r.node_id = c.node_id;
      r.input_bytes = c.input_bytes;
      first(r.t_alloc);
      break;
    case TaskKind::kSent: first(r.t_sent); break;
    case TaskKind::kReceived: first(r.t_recv); break;
    case TaskKind::kDone:
    case TaskKind::kFailed:
      first(r.t_done);
      break;
    case TaskKind::kTimeout:
    case TaskKind::kVoid:
      break;
  }
}

void TaskMerger::restore(std::vector<TaskRow> rows) {
  for (auto& r : rows) {
    auto it = index_.find(r.task_id);
    if (it == index_.end()) {
      if (rows_.size() >= cap_) {
        ++dropped_;
        continue;
      }
      index_.emplace(r.task_id, rows_.size());
      rows_.push_back(std::move(r));
      continue;
    }
    // 已有新变化：时间取较早的，状态按覆盖规则，分配信息以有为准
    TaskRow& cur = rows_[it->second];
    if (task_state_overrides(r.state, cur.state)) {
      // cur 更新：保持
    } else {
      cur.state = r.state;
    }
    if (r.has_alloc) {
      cur.has_alloc = true;
      cur.node_id = r.node_id;
      cur.input_bytes = r.input_bytes;
    }
    auto earlier = [](std::optional<int64_t>& a, const std::optional<int64_t>& b) {
      if (b && (!a || *b < *a)) a = b;
    };
    earlier(cur.t_alloc, r.t_alloc);
    earlier(cur.t_sent, r.t_sent);
    earlier(cur.t_recv, r.t_recv);
    earlier(cur.t_done, r.t_done);
  }
}

std::vector<TaskRow> TaskMerger::take() {
  std::vector<TaskRow> out;
  out.swap(rows_);
  index_.clear();
  return out;
}

}  // namespace lm::manager
