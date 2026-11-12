#pragma once

#include <grpcpp/support/status.h>

#include <memory>

#include "monitor/metric_collector.h"
#include "monitor_info.grpc.pb.h"
#include "monitor_info.pb.h"

namespace monitor {

// gRPC 服务实现类 - 实时采集监控数据
class GrpcManagerImpl : public monitor::proto::GrpcManager::Service {
 public:
  GrpcManagerImpl();
  virtual ~GrpcManagerImpl();

  // SetMonitorInfo 已移除 - Server 端现在本地采集数据

  // 实时采集并返回监控数据
  ::grpc::Status GetMonitorInfo(::grpc::ServerContext* context,
                                const ::google::protobuf::Empty* request,
                                ::monitor::proto::MonitorInfo* response);

 private:
  std::unique_ptr<MetricCollector> collector_;
};

}  // namespace monitor
