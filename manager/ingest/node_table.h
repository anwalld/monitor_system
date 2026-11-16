#pragma once

// 节点表：Manager 调度状态的一部分，由一把互斥锁保护。持锁期间只做内存操作，不做任何 IO。
// 每次接受上报时在同一把锁内重算分数和关键指标剔除状态（M5）；在途账（InflightLedger）也由这把锁保护，
// 选择时直接读在途因子，分配与记账在一次加锁内完成。M6 起每台节点有熔断器状态（慢启动、正常、剔除、急救），
// 状态转移也在这把锁内完成，锁外只打日志、写 Redis、启动急救会话。

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "breaker/breaker_params.h"
#include "breaker/node_state.h"
#include "inflight/inflight_ledger.h"
#include "monitor.pb.h"
#include "scoring/eviction.h"
#include "scoring/score.h"
#include "scoring/scoring_params.h"
#include "scoring/selector.h"
#include "storage/events.h"

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
  BreakerState br;
  double prob = 0;  // 最近一次 tick_states 算出的选中概率（非候选为 0）
  // 负载预测（M9，Advisor.PushPredictions 推来）：因子已限幅；pred_until_mono_ms < 0 表示从未收到
  double pred_factor = 1;
  double pred_cpu = 0;
  int64_t pred_until_mono_ms = -1;
};

// Advisor（AI 预测）参数（M9），manager.yaml 的 advisor 段。
struct AdvisorParams {
  bool enabled = true;
  double min_factor = 0.5;        // f_pred 下限（上限恒为 1）
  int64_t default_ttl_ms = 10000; // 推送的 ttl_ms 为 0 时使用
  int64_t max_ttl_ms = 60000;
  static AdvisorParams from_config(const lm::Config& cfg);
  void validate() const;
};

// 一条预测（已从 proto 拷出）。
struct PredictionItem {
  std::string node_id;
  double cpu_busy_30s = 0;
  double factor = 1;
};

struct PredictionResult {
  int applied = 0;
  int unknown = 0;  // 节点表里没有的节点
  int64_t ttl_ms = 0;
};

// 看板用的节点快照（一次加锁拷贝出来，锁外序列化）。
struct NodeView {
  NodeState st;
  int64_t age_ms = 0;
  bool online = false;
  double f_slow = 1;
  double f_pred = 1;                 // 当前生效的预测因子（无预测或过期为 1）
  std::optional<double> pred_cpu;    // 预测的 30 秒后 CPU；无预测或过期为空
  InflightSnapshot inflight;
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
  // 节点状态（M6）
  NodeMode mode = NodeMode::kSlowStart;
  std::string mode_reason;
  int64_t mode_since_wall_ms = 0;
  double f_slow = 1;
  int probe_fail_streak = 0;
  double prob = 0;
  double f_pred = 1;                  // 预测因子（M9）
  std::optional<double> pred_cpu;
  std::optional<StateChange> change;  // 这条上报引起的状态变化
  // 只写状态字段（状态变化时由调度主循环推送；不写时间序列和流）
  bool state_only = false;
  bool events_only = false;          // 只有事件（急救结束），不写任何节点字段
  std::vector<NodeEvent> events;     // 由存储线程 XADD stream:events 并转给 MySQL
  std::string node_id;  // state_only 时使用
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
  std::size_t rescue = 0;
  std::size_t candidates = 0;
};

// 急救批结算后的概率（按节点回到候选计算）。
struct RescueProbe {
  bool found = false;
  double prob = 0;
  int candidates = 0;  // N（含该节点）
  double timeout_count = 0;
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
    BreakerParams breaker;
    AdvisorParams advisor;
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
    std::size_t slow_start = 0;
    std::size_t normal = 0;
    std::size_t evicted = 0;
    std::size_t rescue = 0;
  };
  Counts counts(int64_t now_mono_ms) const;

  std::optional<NodeState> get(const std::string& node_id) const;
  // 距最近一次上报经过的毫秒数；未知节点返回空。
  std::optional<int64_t> age_ms(const std::string& node_id, int64_t now_mono_ms) const;
  std::vector<std::string> node_ids() const;

  // 按权重随机选一台：候选为未剔除、未失联（距上次上报 ≤ lost_after_ms）、分数 > 0 的节点，
  // 按 node_id 排序后做前缀和，u ∈ [0, 1) 由调用方在锁外生成。只做内存操作。
  // policy 为对照策略（M10）时按 select_baseline_locked 选择（会推进轮询状态）。
  SelectResult select(int64_t now_mono_ms, double u, Policy policy = Policy::kScore) const;

  // 选择并记账（一次加锁）。选中时 task_id 记入在途账（状态 ALLOCATED）。
  SelectResult allocate(int64_t now_mono_ms, double u, const std::string& task_id,
                        uint64_t input_bytes, Policy policy = Policy::kScore);
  InflightLedger::MarkResult mark_sent(const std::string& task_id, int64_t now_mono_ms);
  InflightLedger::EventResult on_event(const std::string& task_id, lm::TaskEvent::Kind kind,
                                       int64_t now_mono_ms);
  // 调度主循环：每 100 ms 处理超时链表头；每秒更新基线、衰减 T。
  void tick(int64_t now_mono_ms, std::vector<InflightLedger::Expired>* expired);
  void tick_second(double dt_s = 1.0);
  InflightLedger::Totals inflight_totals() const;
  InflightSnapshot inflight_snapshot(const std::string& node_id) const;

  // ---- 节点状态机（M6）----
  // 每 100 ms：修剪确认窗口、计算灰色条件、评估所有节点的转移；收集状态变化与要启动的急救。
  void tick_states(int64_t now_mono_ms, int64_t now_wall_ms, std::vector<StateChange>* changes,
                   std::vector<RescueStart>* rescues);
  // 低频探测结果；可能立即引起状态变化。
  std::optional<StateChange> on_probe(const std::string& node_id, bool ok, int64_t now_mono_ms,
                                      int64_t now_wall_ms);
  // 需要探测的节点（新鲜、不在急救中）：(node_id, addr)。
  std::vector<std::pair<std::string, std::string>> probe_targets(int64_t now_mono_ms) const;
  // 急救批结算：成功时 T 乘以 t_decay；返回节点回到候选时的选中概率与 N。
  RescueProbe rescue_batch(const std::string& node_id, bool ok, int64_t now_mono_ms);
  // 急救结束：新鲜则慢启动，否则剔除（lost）。
  std::optional<StateChange> end_rescue(const std::string& node_id, int64_t now_mono_ms,
                                        int64_t now_wall_ms);
  std::optional<NodeMode> mode(const std::string& node_id) const;
  double f_slow(const std::string& node_id, int64_t now_mono_ms) const;
  // 全部节点的快照（按 node_id 排序）。
  std::vector<NodeView> views(int64_t now_mono_ms) const;

  // ---- 负载预测（M9）----
  // 记录 AI 推来的预测：因子限幅到 [advisor.min_factor, 1]，过期时刻 = now + ttl（ttl 为 0 用默认，上限 max_ttl）。
  // 一次加锁，只做赋值；未知节点忽略并计数。advisor.enabled 为 false 时不记录。
  PredictionResult set_predictions(const std::vector<PredictionItem>& items, int64_t ttl_ms,
                                   int64_t now_mono_ms);
  // 当前生效的预测因子（过期或没有为 1）；未知节点返回空。
  std::optional<double> f_pred(const std::string& node_id, int64_t now_mono_ms) const;

  const Options& options() const { return opt_; }

 private:
  SelectResult select_locked(int64_t now_mono_ms, double u, bool use_pred = true) const;
  // 对照策略（rr、wrr）：候选只要求新鲜、低频探测未到剔除线，不看分数、剔除、急救、慢启动与修正因子。
  SelectResult select_baseline_locked(int64_t now_mono_ms, Policy policy) const;
  bool fresh(const NodeState& n, int64_t now_mono_ms) const {
    return now_mono_ms - n.last_seen_mono_ms <= opt_.scoring.lost_after_ms;
  }
  bool eligible(const NodeState& n, int64_t now_mono_ms) const;
  static bool pred_active(const NodeState& n, int64_t now_mono_ms) {
    return n.pred_until_mono_ms >= 0 && now_mono_ms <= n.pred_until_mono_ms;
  }
  Factors factors_for(const NodeState& n, int64_t now_mono_ms, bool as_normal) const;
  std::string evict_reason(const NodeState& n, int64_t now_mono_ms) const;
  void set_mode(NodeState& n, NodeMode m, std::string reason, int64_t now_mono_ms,
                int64_t now_wall_ms, std::vector<StateChange>* changes);
  // 不涉及急救触发的转移（apply、探测时立即调用）。
  void evaluate(NodeState& n, int64_t now_mono_ms, int64_t now_wall_ms,
                std::vector<StateChange>* changes);

  const Options opt_;
  mutable std::mutex mu_;
  std::unordered_map<std::string, NodeState> nodes_;
  InflightLedger ledger_;
  // 对照策略的状态（M10），锁内读写；select() 为 const 接口，所以是 mutable
  mutable uint64_t rr_next_ = 0;
  mutable std::unordered_map<std::string, int64_t> wrr_current_;
};

// 从 gRPC peer 字符串（"ipv4:10.0.0.1:5000"、"ipv6:[::1]:5000"、"ipv6:%5B::1%5D:5000"）中取出 IP。
std::string parse_peer_ip(std::string_view peer);

// 拼接 "ip:port"；IPv6 地址加方括号。
std::string join_host_port(std::string_view ip, uint16_t port);

}  // namespace lm::manager
