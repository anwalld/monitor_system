#pragma once

// 在途任务账：Allocate → MarkSent → RECEIVED → DONE/FAILED 四步，两条按截止时间有序的超时链表
// （确认 30 s、执行 300 s），每节点在途量、未确认数、超时计数 T，以及由此得到的 f_inflight、f_timeout。
// 本类不加锁，由 NodeTable 的锁保护；时钟由调用方传入，便于用模拟时钟测试。

#include <cstdint>
#include <list>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "lm/config.h"
#include "monitor.pb.h"
#include "scoring/selector.h"

namespace lm::manager {

struct InflightParams {
  int64_t ack_timeout_ms = 30 * 1000;
  int64_t exec_timeout_ms = 300 * 1000;
  double lambda = 0.5;                  // f_timeout = exp(−λ·T)
  double timeout_half_life_s = 600;     // T 的半衰期
  double baseline_tau_s = 60;           // 在途基线指数平滑的时间常数
  double base_tasks_floor = 2;          // 基线下限，避免接近 0 时比值爆炸
  double base_bytes_floor = 1 << 20;

  static InflightParams from_config(const lm::Config& cfg);  // inflight 段；不合法抛 ConfigError
  void validate() const;
};

// 一个节点的在途状态（给 Redis 与日志）。
struct InflightSnapshot {
  int64_t inflight_tasks = 0;
  int64_t inflight_bytes = 0;
  uint64_t unconfirmed = 0;
  double timeout_count = 0;  // T
  double f_inflight = 1;
  double f_timeout = 1;
};

class InflightLedger {
 public:
  enum class State { kAllocated, kSent, kRunning };
  enum class MarkResult { kOk, kNotFound };
  enum class EventResult { kApplied, kIgnored, kLate };

  struct Expired {
    enum class Kind { kVoided, kUnconfirmed, kExecTimeout };
    std::string task_id;
    std::string node_id;
    Kind kind;
  };

  struct NodeCounters {
    uint64_t assigned_total = 0;  // 累计分配，不减
    int64_t total = 0;            // 开发文档的“总数”：已分配且未被确认超时作废
    int64_t inflight_tasks = 0;
    int64_t inflight_bytes = 0;
    uint64_t unconfirmed = 0;     // 累计：MarkSent 后 30 s 没有 RECEIVED
    uint64_t voided = 0;          // 累计：没有 MarkSent，30 s 后作废
    uint64_t exec_timeouts = 0;   // 累计
    double timeout_count = 0;     // T，按半衰期衰减
    double base_tasks = 0;
    double base_bytes = 0;
  };

  struct Totals {
    std::size_t inflight = 0;  // 账上的任务数
    uint64_t voided = 0;
    uint64_t unconfirmed = 0;
    uint64_t exec_timeouts = 0;
    uint64_t late_events = 0;
  };

  explicit InflightLedger(InflightParams p = {}) : p_(p) {}

  // 分配后记账；task_id 已存在时返回 false 且不改动。
  bool add(const std::string& task_id, const std::string& node_id, uint64_t input_bytes,
           int64_t now_ms);
  MarkResult mark_sent(const std::string& task_id, int64_t now_ms);
  EventResult on_event(const std::string& task_id, lm::TaskEvent::Kind kind, int64_t now_ms);
  // 处理两条链表头上已过期的任务；过期项追加到 out（可为空）。
  void tick(int64_t now_ms, std::vector<Expired>* out);
  // 每秒调用一次：更新基线、衰减 T。dt_s 为距上次调用的秒数。
  void tick_second(double dt_s = 1.0);

  Factors factors(const std::string& node_id) const;  // 只填 inflight、timeout
  InflightSnapshot snapshot(const std::string& node_id) const;
  const NodeCounters* node(const std::string& node_id) const;
  bool has_task(const std::string& task_id) const { return tasks_.count(task_id) != 0; }
  std::optional<State> state(const std::string& task_id) const;
  std::optional<int64_t> deadline(const std::string& task_id) const;
  Totals totals() const;
  const InflightParams& params() const { return p_; }

 private:
  struct Task {
    std::string node_id;
    uint64_t bytes = 0;
    State state = State::kAllocated;
    int64_t deadline = 0;
    std::list<std::string>::iterator it;  // 在 ack_ 或 exec_ 中的位置
  };

  void release(Task& t);  // 在途任务数减一、字节数减去 bytes
  void erase(std::unordered_map<std::string, Task>::iterator it);

  const InflightParams p_;
  std::unordered_map<std::string, Task> tasks_;
  std::list<std::string> ack_;   // ALLOCATED、SENT，按截止时间升序
  std::list<std::string> exec_;  // RUNNING，按截止时间升序
  std::unordered_map<std::string, NodeCounters> nodes_;
  uint64_t late_events_ = 0;
};

}  // namespace lm::manager
