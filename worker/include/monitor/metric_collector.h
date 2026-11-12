#pragma once

#include <memory>
#include <string>
#include <vector>

#include "monitor/monitor_inter.h"
#include "monitor_info.pb.h"

namespace monitor {

class MetricCollector {
 public:
  MetricCollector();
  ~MetricCollector();

  // 采集所有指标并填充到 MonitorInfo
  void CollectAll(monitor::proto::MonitorInfo* monitor_info);

 private:
  std::vector<std::unique_ptr<MonitorInter>> monitors_;
  std::string hostname_;
};

}  // namespace monitor
