#pragma once

// 节点事件：状态转移与急救开始、结束。写入 stream:events（看板、AI 读取）与 MySQL node_event。

#include <cstdint>
#include <string>
#include <vector>

#include "breaker/node_state.h"

namespace lm::manager {

struct NodeEvent {
  std::string node_id;
  std::string kind;     // slow_start / normal / evict / rescue_start / rescue_end
  int64_t ts_ms = 0;    // 墙上时间
  std::string detail;   // JSON 对象
  std::string id;       // stream:events 的 entry id（XADD 之后填入；Redis 不可用时为空）
};

// 一次状态变化对应的事件（新节点登记用 from == to == slow_start 表示）。
// 新节点的 addr、ncpu 由调用方传入（其他情况忽略）。
std::vector<NodeEvent> events_from_change(const StateChange& c, const std::string& addr = "",
                                          uint32_t ncpu = 0);

// 急救结束的统计。
struct RescueEnd {
  std::string node_id;
  int64_t ts_ms = 0;
  std::string result;  // reached / timeout / canceled / gone
  int64_t ms = 0;
  int batches_ok = 0;
  int batches_fail = 0;
  uint64_t sent = 0, ok = 0, timeouts = 0, late = 0, events = 0;
  double prob = 0, target = 0, t_before = 0, t_after = 0;
  std::string to;  // 结束后转入的状态
};

NodeEvent event_from_rescue_end(const RescueEnd& r);

}  // namespace lm::manager
