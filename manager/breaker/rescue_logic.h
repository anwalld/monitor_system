#pragma once

// 急救判定逻辑（与 gRPC 无关，时钟由调用方传入）：
//   RescueTracker  待确认队列、单次超时、迟到、网络事件、批结算
//   RescueTarget   目标 = max(0, 1/N − ε·(2^k − 1))，k 为连续失败批数
//   Backoff        急救退避：间隔 min(min × 2^floor(level), max)，level 按半衰期衰减

#include <cstdint>
#include <deque>
#include <utility>

#include "breaker_params.h"

namespace lm::manager {

struct RescueStats {
  uint64_t sent = 0;
  uint64_t ok = 0;
  uint64_t timeouts = 0;
  uint64_t late = 0;
  uint64_t events = 0;  // 网络事件：一段连续超时算一次
  int batches_ok = 0;
  int batches_fail = 0;
};

class RescueTracker {
 public:
  explicit RescueTracker(int64_t timeout_us) : timeout_us_(timeout_us) {}

  void on_send(uint64_t seq, int64_t now_us);
  void on_response(uint64_t seq, int64_t now_us);
  // 队头发出超过 timeout 仍无回应的出队记超时，直到队头未超时。
  void check(int64_t now_us);
  // 结算一批：本批没有超时返回 true。清零批内计数。
  bool settle();

  const RescueStats& stats() const { return stats_; }
  std::size_t pending() const { return pending_.size(); }
  bool in_outage() const { return outage_; }

 private:
  void timeout_one();
  void success_one();

  const int64_t timeout_us_;
  std::deque<std::pair<uint64_t, int64_t>> pending_;  // (seq, 发送时刻)
  bool outage_ = false;
  uint64_t batch_timeouts_ = 0;
  RescueStats stats_;
};

class RescueTarget {
 public:
  RescueTarget(double epsilon_factor) : epsilon_factor_(epsilon_factor) {}
  // 记一批结果，返回新的目标。n 为候选数（含急救中的节点）。
  double on_batch(bool ok, int n);
  static double target_for(int k, int n, double epsilon_factor);
  // p 达到目标：允许 1% 相对误差。T 每批只乘 0.9，概率只能渐近 1/N（分数相同时永远差一点）。
  static constexpr double kReachTolerance = 0.01;
  static bool reached(double prob, double target);
  int k() const { return k_; }

 private:
  const double epsilon_factor_;
  int k_ = 0;
};

class Backoff {
 public:
  bool allowed(int64_t now_ms) const { return now_ms >= next_ms_; }
  // 进入急救时调用：衰减 level，设置下一次允许急救的时刻，level 加 1。返回本次间隔。
  int64_t trigger(int64_t now_ms, const RescueParams& p);
  double level_at(int64_t now_ms, const RescueParams& p) const;
  int64_t next_ms() const { return next_ms_; }

 private:
  double level_ = 0;
  int64_t level_ts_ = 0;
  int64_t next_ms_ = 0;
};

}  // namespace lm::manager
