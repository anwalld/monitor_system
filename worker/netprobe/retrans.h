#pragma once

// TCP 重传率（纯逻辑）：按连接 cookie 记住上次的累计重传段与发送段，做差后按本地地址归到网卡。

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace lm::worker {

struct TcpConnSample {
  uint64_t cookie = 0;
  uint32_t local_ip = 0;  // 网络字节序
  uint32_t total_retrans = 0;
  uint32_t segs_out = 0;
};

class RetransTracker {
 public:
  // ip_to_nic：本地 IPv4 → 网卡编号；nnic：网卡数。
  RetransTracker(std::unordered_map<uint32_t, int> ip_to_nic, int nnic)
      : ip_to_nic_(std::move(ip_to_nic)), nnic_(nnic) {}

  // 用本次快照更新，返回每块网卡本周期的重传率（ΣΔ重传 / ΣΔ发送段，分母为 0 时 0）。
  std::vector<double> update(const std::vector<TcpConnSample>& conns);

 private:
  struct Prev {
    uint32_t retrans;
    uint32_t segs;
  };
  std::unordered_map<uint32_t, int> ip_to_nic_;
  int nnic_;
  std::unordered_map<uint64_t, Prev> prev_;
};

}  // namespace lm::worker
