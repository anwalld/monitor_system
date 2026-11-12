#pragma once

#include <chrono>
#include <string>
#include <unordered_map>

#include "monitor/monitor_inter.h"
#include "monitor_info.pb.h"

namespace monitor {
class NetMonitor : public MonitorInter {
  struct NetInfo {
    std::string name;
    uint64_t rcv_bytes;
    uint64_t rcv_packets;
    uint64_t snd_bytes;
    uint64_t snd_packets;
    uint64_t err_in;
    uint64_t err_out;
    uint64_t drop_in;
    uint64_t drop_out;
    std::chrono::steady_clock::time_point timepoint;
  };

 public:
  NetMonitor() {}
  void UpdateOnce(monitor::proto::MonitorInfo* monitor_info) override;
  void Stop() override {}

 private:
  std::unordered_map<std::string, NetInfo> last_net_info_;
};

}  // namespace monitor