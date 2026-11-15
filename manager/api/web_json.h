#pragma once

// 看板接口的纯逻辑：节点与事件的 JSON、曲线降采样、token 检查、Stream id 比较。
// 不依赖 Drogon，总是编译、可单测。

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ingest/node_table.h"
#include "monitor.pb.h"

namespace lm::manager {

// 单个节点对象（REST /api/nodes 与 WebSocket node_update 共用）。
std::string node_json(const NodeView& v);
// [node, node, …]
std::string nodes_array_json(const std::vector<NodeView>& vs);

// stream:events 的一条 entry → {"id","node","kind","ts","detail":{…}}；with_type 时加 "type":"event"。
using StreamFields = std::vector<std::pair<std::string, std::string>>;
std::string event_json(const std::string& id, const StreamFields& fields, bool with_type);
// entry 里的某个字段（没有为空串）。
std::string field_of(const StreamFields& f, const std::string& k);

// 降采样后的一个点。
struct SeriesPoint {
  int64_t ts_ms = 0;  // 桶起点
  int n = 0;
  double cpu = 0, mem = 0, disk = 0;
  std::optional<double> runq_p99_us;  // 桶内最大
  std::optional<double> rtt_ms, timeout_rate, retrans_rate;  // 各网卡平均后桶内平均
  double rx_bps = 0, tx_bps = 0;      // 各网卡之和，桶内平均
};

// 按 step_ms 分桶（floor(ts/step)×step），输入不要求有序，输出按时间升序，空桶不输出。
std::vector<SeriesPoint> downsample(const std::vector<lm::Sample>& samples, int64_t step_ms);
std::string series_json(const std::string& node_id, int64_t step_ms,
                        const std::vector<SeriesPoint>& pts);

// token 检查：expected 为空时放行；比较不提前返回。
bool token_ok(const std::string& expected, const std::string& got);
// 从 "Bearer xxx" 里取 token（不是这个格式返回空）。
std::string bearer_token(const std::string& authorization);

// Stream id（"ms-seq"）比较：a < b。格式不对的当作 0-0。
bool stream_id_less(const std::string& a, const std::string& b);

}  // namespace lm::manager
