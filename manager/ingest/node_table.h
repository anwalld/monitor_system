#pragma once

// 节点表：Manager 调度状态的一部分，由一把互斥锁保护。持锁期间只做内存操作，不做任何 IO。
// 每次接受上报时在同一把锁内重算分数和关键指标剔除状态（M5）；在途账（InflightLedger）也由这把锁保护，
// 选择时直接读在途因子，分配与记账在一次加锁内完成。

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "inflight/inflight_ledger.h"
#include "monitor.pb.h"
#include "scoring/eviction.h"
#include "scoring/score.h"
#include "scoring/scoring_params.h"
#include "scoring/selector.h"

namespace lm::manager {

struct NodeState {
  std::string node_id;
  std::string addr;            // Worker gRPC 地址：上报对端 IP + worker_port
  uint32_t ncpu = 0;
  int64_t first_seen_wall_ms = 0;
  int64_t last_seen_wall_ms = 0;
  int64_t last_seen_mono_ms = 0;
  uint64_t last_seq = 0;
  lm::Sample latest;
  ScoreBreakdown score;
  EvictionTracker evict;
  bool hinted_runq_missing = false;  // 已提示过“未上报调度延迟”
  bool hinted_net_missing = false;   // 已提示过“没有网卡数据”
};

// 一次被接受的上报，交给存储线程写热数据（锁外拷贝出来）。
struct AcceptedSample {
  std::string addr;
  uint32_t ncpu = 0;
  int64_t recv_wall_ms = 0;
  bool new_node = false;
  lm::Sample sample;
  ScoreBreakdown score;
  bool evicted = false;
  std::string evict_reason;
  bool evicted_changed = false;     // 这条上报使剔除状态发生变化（锁外打日志）
  bool hint_runq_missing = false;   // 该节点首次出现该缺失（锁外打日志）
  bool hint_net_missing = false;
  InflightSnapshot inflight;
};

// 一次选择的结果。pick 为空表示没有候选或总权重为 0。
struct SelectResult {
  struct Pick {
    std::string node_id;
    std::string addr;
    double weight = 0;
    double prob = 0;
  };
  std::optional<Pick> pick;
  std::size_t total = 0;
  std::size_t evicted = 0;
  std::size_t lost = 0;
  std::size_t candidates = 0;
};

class NodeTable {
 public:
  enum class Result { kAccepted, kStale, kInvalid };

  struct Options {
    uint16_t worker_port = 50052;
    int64_t restart_gap_ms = 3000;  // 距上次上报超过该时间、seq 变小视为 Worker 重启
    int64_t online_window_ms = 3000;
    std::size_t max_node_id_len = 64;
    ScoringParams scoring;
    InflightParams inflight;
  };

  explicit NodeTable(Options opt) : opt_(std::move(opt)), ledger_(opt_.inflight) {}

  // 应用一条上报。peer_ip 为上报连接对端 IP。被接受时填充 out。
  Result apply(const lm::Sample& s, std::string_view peer_ip, int64_t now_mono_ms,
               int64_t now_wall_ms, AcceptedSample* out);

  // 参数是否合法（node_id 非空、不超长）；不合法时给出原因。
  std::optional<std::string> validate(const lm::Sample& s) const;

  struct Counts {
    std::size_t online = 0;
    std::size_t total = 0;
  };
  Counts counts(int64_t now_mono_ms) const;

  std::optional<NodeState> get(const std::string& node_id) const;
  // 距最近一次上报经过的毫秒数；未知节点返回空。
  std::optional<int64_t> age_ms(const std::string& node_id, int64_t now_mono_ms) const;
  std::vector<std::string> node_ids() const;

  // 按权重随机选一台：候选为未剔除、未失联（距上次上报 ≤ lost_after_ms）、分数 > 0 的节点，
  // 按 node_id 排序后做前缀和，u ∈ [0, 1) 由调用方在锁外生成。只做内存操作。
  SelectResult select(int64_t now_mono_ms, double u) const;

  // 选择并记账（一次加锁）。选中时 task_id 记入在途账（状态 ALLOCATED）。
  SelectResult allocate(int64_t now_mono_ms, double u, const std::string& task_id,
                        uint64_t input_bytes);
  InflightLedger::MarkResult mark_sent(const std::string& task_id, int64_t now_mono_ms);
  InflightLedger::EventResult on_event(const std::string& task_id, lm::TaskEvent::Kind kind,
                                       int64_t now_mono_ms);
  // 调度主循环：每 100 ms 处理超时链表头；每秒更新基线、衰减 T。
  void tick(int64_t now_mono_ms, std::vector<InflightLedger::Expired>* expired);
  void tick_second(double dt_s = 1.0);
  InflightLedger::Totals inflight_totals() const;
  InflightSnapshot inflight_snapshot(const std::string& node_id) const;

  const Options& options() const { return opt_; }

 private:
  SelectResult select_locked(int64_t now_mono_ms, double u) const;

  const Options opt_;
  mutable std::mutex mu_;
  std::unordered_map<std::string, NodeState> nodes_;
  InflightLedger ledger_;
};

// 从 gRPC peer 字符串（"ipv4:10.0.0.1:5000"、"ipv6:[::1]:5000"、"ipv6:%5B::1%5D:5000"）中取出 IP。
std::string parse_peer_ip(std::string_view peer);

// 拼接 "ip:port"；IPv6 地址加方括号。
std::string join_host_port(std::string_view ip, uint16_t port);

}  // namespace lm::manager
