#pragma once

#include <string>
#include "monitor/monitor_inter.h"

namespace monitor {

/**
 * 主机标识信息监控器
 * 
 * 采集服务器的标识信息，用于调度服务器识别和定位：
 * - hostname: 主机名，通过 gethostname() 获取
 * - ip_address: IP 地址，遍历网卡获取非 loopback 的 IP
 * 
 * 这些信息对于分布式负载均衡场景至关重要：
 * 调度服务器根据性能指标选出最优服务器后，
 * 可以直接使用 IP 信息将请求路由到该服务器。
 */
class HostInfoMonitor : public MonitorInter {
 public:
  HostInfoMonitor() = default;
  ~HostInfoMonitor() override = default;

  void UpdateOnce(monitor::proto::MonitorInfo* monitor_info) override;
  void Stop() override {}

 private:
  /**
   * 获取主机名
   * 使用 gethostname() 系统调用
   * @return 主机名字符串
   */
  std::string GetHostname();

  /**
   * 获取主网卡 IP 地址
   * 遍历 /sys/class/net/ 目录，过滤 lo 和虚拟网卡，
   * 获取第一个物理网卡的 IPv4 地址
   * @return IP 地址字符串
   */
  std::string GetPrimaryIpAddress();

  std::string cached_hostname_;     // 缓存的主机名
  std::string cached_ip_;           // 缓存的 IP 地址
  bool info_cached_ = false;        // 是否已缓存（主机信息通常不变）
};

}  // namespace monitor
