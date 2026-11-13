#pragma once

// 用 Netlink RTM_GETROUTE 转储所有 IPv4 路由表，找每块网卡的默认网关（仅 Linux）。

#include <cstdint>
#include <map>

namespace lm::worker {

// ifindex → 默认网关（网络字节序）。主表（254）优先，其次编号最小的表。非 Linux 或失败时返回空。
std::map<unsigned, uint32_t> default_gateways();

}  // namespace lm::worker
