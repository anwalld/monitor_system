#pragma once

// 网卡探测配置：net.nics（网卡名 + 1~3 个 IPv4 目标）、探测周期与超时。
// 不配置 net.nics 时由 discover_nics() 自动选择物理网卡并以默认网关为目标（仅 Linux）。

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "lm/config.h"

namespace lm::worker {

struct NicSpec {
  std::string name;
  std::vector<uint32_t> targets;  // IPv4，网络字节序
  std::vector<std::string> target_strs;
};

struct NetProbeConfig {
  std::vector<NicSpec> nics;  // 为空表示自动选择
  int interval_ms = 200;
  int timeout_ms = 1000;
};

inline constexpr int kMaxTargetsPerNic = 3;

// 解析 net.*；nic_exists 判断网卡是否存在（默认查 /sys/class/net）。出错抛 ConfigError。
NetProbeConfig parse_net_config(const lm::Config& cfg,
                                const std::function<bool(const std::string&)>& nic_exists = {});

// 解析点分 IPv4；失败返回 false。
bool parse_ipv4(const std::string& s, uint32_t& out);
std::string ipv4_str(uint32_t addr);

// 自动选择：UP、有 IPv4、/sys/class/net/<名>/device 存在的网卡，目标为任一路由表中的默认网关（主表优先）。
// 找不到网关的网卡写进 skipped（附原因）。非 Linux 返回空。
std::vector<NicSpec> discover_nics(std::vector<std::string>* skipped);

}  // namespace lm::worker
