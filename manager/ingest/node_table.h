#pragma once

// 节点表：Manager 调度状态的一部分，由一把互斥锁保护。持锁期间只做内存操作，不做任何 IO。
// M5 起评分、剔除、在途账会挂在同一张表上。

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "monitor.pb.h"

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
};

// 一次被接受的上报，交给存储线程写热数据（锁外拷贝出来）。
struct AcceptedSample {
  std::string addr;
  uint32_t ncpu = 0;
  int64_t recv_wall_ms = 0;
  bool new_node = false;
  lm::Sample sample;
};

class NodeTable {
 public:
  enum class Result { kAccepted, kStale, kInvalid };

  struct Options {
    uint16_t worker_port = 50052;
    int64_t restart_gap_ms = 3000;  // 距上次上报超过该时间、seq 变小视为 Worker 重启
    int64_t online_window_ms = 3000;
    std::size_t max_node_id_len = 64;
  };

  explicit NodeTable(Options opt) : opt_(opt) {}

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

 private:
  const Options opt_;
  mutable std::mutex mu_;
  std::unordered_map<std::string, NodeState> nodes_;
};

// 从 gRPC peer 字符串（"ipv4:10.0.0.1:5000"、"ipv6:[::1]:5000"、"ipv6:%5B::1%5D:5000"）中取出 IP。
std::string parse_peer_ip(std::string_view peer);

// 拼接 "ip:port"；IPv6 地址加方括号。
std::string join_host_port(std::string_view ip, uint16_t port);

}  // namespace lm::manager
