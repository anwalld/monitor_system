#pragma once

// 网卡链路质量汇总：每周期把探测结果、TCP 重传率、流量组装成 lm::NicStat 列表（按配置顺序）。
// 数据源通过 Sources 注入，便于测试失败沿用逻辑；create() 构造真实的 Linux 数据源。

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "monitor.pb.h"
#include "nic_config.h"
#include "probe_state.h"
#include "retrans.h"
#include "traffic.h"

namespace lm::worker {

class NicMonitor {
 public:
  struct Sources {
    std::function<std::vector<ProbePeriod>()> probe;                    // 每网卡一项
    std::function<std::optional<std::vector<TcpConnSample>>()> tcp;    // 失败返回 nullopt
    std::function<std::optional<uint64_t>(std::size_t nic, bool rx)> bytes;
    std::function<int64_t()> now_ns;
    std::unordered_map<uint32_t, int> ip_to_nic;
    std::function<void()> stop;  // 停止探测线程等
  };

  NicMonitor(std::vector<std::string> names, Sources src);
  ~NicMonitor();

  // 构造真实数据源（Prober、SockDiag、sysfs 计数），并启动探测线程。失败抛 std::runtime_error。
  static std::unique_ptr<NicMonitor> create(const NetProbeConfig& cfg, const std::vector<NicSpec>& nics);

  std::vector<lm::NicStat> collect();

 private:
  std::vector<std::string> names_;
  Sources src_;
  RetransTracker retrans_;
  std::vector<RateCounter> rx_, tx_;
  std::vector<double> last_retrans_;
  std::vector<uint64_t> last_rx_, last_tx_;
  int tcp_fails_ = 0;
  int bytes_fails_ = 0;
};

}  // namespace lm::worker
