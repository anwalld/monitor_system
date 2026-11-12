#include "rpc/rpc_client.h"

#include <chrono>
#include <iostream>

namespace monitor {

RpcClient::RpcClient(const std::string& host_address)
    : host_address_(host_address) {
  auto channel =
      grpc::CreateChannel(host_address, grpc::InsecureChannelCredentials());
  stub_ptr_ = monitor::proto::GrpcManager::NewStub(channel);
  query_stub_ = monitor::proto::QueryService::NewStub(channel);
}

RpcClient::~RpcClient() {}

bool RpcClient::GetMonitorInfo(monitor::proto::MonitorInfo* monitor_info) {
  if (!monitor_info) {
    return false;
  }

  ::grpc::ClientContext context;
  ::google::protobuf::Empty request;

  ::grpc::Status status =
      stub_ptr_->GetMonitorInfo(&context, request, monitor_info);

  if (status.ok()) {
    return true;
  } else {
    std::cerr << "Failed to get monitor info from " << host_address_ << ": "
              << status.error_message() << std::endl;
    return false;
  }
}

bool RpcClient::QueryPerformance(
    const monitor::proto::QueryPerformanceRequest& request,
    monitor::proto::QueryPerformanceResponse* response) {
  if (!response) {
    return false;
  }

  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() +
                       std::chrono::seconds(10));
  grpc::Status status =
      query_stub_->QueryPerformance(&context, request, response);
  if (!status.ok()) {
    std::cerr << "Failed to query performance from " << host_address_ << ": "
              << status.error_message() << std::endl;
    return false;
  }
  return true;
}

}  // namespace monitor
