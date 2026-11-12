#include "rpc/grpc_server.h"

#include <iostream>

namespace monitor {

::grpc::Status GrpcServerImpl::SetMonitorInfo(
    ::grpc::ServerContext* context,
    const ::monitor::proto::MonitorInfo* request,
    ::google::protobuf::Empty* response) {
  if (!request) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Empty request");
  }

  std::string hostname = request->name();
  if (hostname.empty() && request->has_host_info()) {
    hostname = request->host_info().hostname();
  }

  if (hostname.empty()) {
    return grpc::Status(grpc::StatusCode::INVALID_ARGUMENT, "Missing hostname");
  }

  // 存储数据
  {
    std::lock_guard<std::mutex> lock(mtx_);
    host_data_[hostname] = {*request, std::chrono::system_clock::now()};
  }

  std::cout << "Received monitor data from: " << hostname << std::endl;

  // 调用回调函数
  if (callback_) {
    callback_(*request);
  }

  return grpc::Status::OK;
}

::grpc::Status GrpcServerImpl::GetMonitorInfo(
    ::grpc::ServerContext* context,
    const ::google::protobuf::Empty* request,
    ::monitor::proto::MonitorInfo* response) {
  // 返回第一个主机的数据（或空）
  std::lock_guard<std::mutex> lock(mtx_);
  if (!host_data_.empty()) {
    *response = host_data_.begin()->second.info;
  }
  return grpc::Status::OK;
}

std::unordered_map<std::string, HostData> GrpcServerImpl::GetAllHostData() {
  std::lock_guard<std::mutex> lock(mtx_);
  return host_data_;
}

bool GrpcServerImpl::GetHostData(const std::string& hostname, HostData* data) {
  std::lock_guard<std::mutex> lock(mtx_);
  auto it = host_data_.find(hostname);
  if (it != host_data_.end()) {
    *data = it->second;
    return true;
  }
  return false;
}

}  // namespace monitor
