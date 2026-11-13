#pragma once

// 节点状态机的数据：四个状态（对应熔断器：正常 = 闭合，剔除 = 断开，急救 = 半开，之后慢启动）。
// 转移逻辑在 NodeTable 里（需要节点表锁内的样本、分数、在途账），这里只放数据和小工具函数。

#include <cstdint>
#include <string>

#include "breaker_params.h"
#include "rescue_logic.h"

namespace lm::manager {

enum class NodeMode { kSlowStart, kNormal, kEvicted, kRescue };

const char* mode_name(NodeMode m);  // slow_start / normal / evicted / rescue

struct BreakerState {
  NodeMode mode = NodeMode::kSlowStart;
  int64_t since_ms = 0;       // 进入当前状态的单调时钟
  int64_t since_wall_ms = 0;  // 同上，墙上时间（写 Redis）
  std::string reason;         // 剔除原因；其他状态为空
  int good_reports = 0;       // 进入剔除后连续“关键指标正常”的上报数
  int probe_fail_streak = 0;
  bool gray = false;          // 最近一次评估时满足未确认比例条件
  Backoff backoff;
};

// 慢启动权重：from + (1 − from) × min(1, elapsed / duration)；其他状态为 1。
double slow_factor(const BreakerState& b, int64_t now_ms, const BreakerParams& p);

// 一次状态变化（锁外打日志、写 Redis）。
struct StateChange {
  std::string node_id;
  NodeMode from = NodeMode::kSlowStart;
  NodeMode to = NodeMode::kSlowStart;
  std::string reason;
  int64_t since_wall_ms = 0;
  double f_slow = 1;
  int probe_fail_streak = 0;
};

// 需要启动的急救。
struct RescueStart {
  std::string node_id;
  std::string addr;
  int candidates = 0;  // N（含该节点）
  double prob = 0;     // 触发时的选中概率
  double timeout_count = 0;
};

}  // namespace lm::manager
