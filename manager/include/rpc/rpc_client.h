#pragma once

#include <string>

#include <grpc/grpc.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/grpcpp.h>

#include "monitor_info.grpc.pb.h"
#include "monitor_info.pb.h"
#include "query_api.grpc.pb.h"

namespace monitor {

// RPC 客户端 - 获取监控数据及查询 Manager 中的历史记录
class RpcClient {
 public:
  explicit RpcClient(const std::string& host_address = "localhost:50051");
  ~RpcClient();

  // 从远程主机获取监控数据
  bool GetMonitorInfo(monitor::proto::MonitorInfo* monitor_info);

  // 按主机和时间范围查询历史性能数据
  bool QueryPerformance(const monitor::proto::QueryPerformanceRequest& request,
                        monitor::proto::QueryPerformanceResponse* response);

  // 获取连接的主机地址
  const std::string& GetHostAddress() const { return host_address_; }

 private:
  std::unique_ptr<monitor::proto::GrpcManager::Stub> stub_ptr_;
  std::unique_ptr<monitor::proto::QueryService::Stub> query_stub_;
  std::string host_address_;
};

}  // namespace monitor
