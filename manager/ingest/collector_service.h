#pragma once

// Collector gRPC 服务：接收 Worker 上报与任务事件。
// Report 在节点表锁内更新内存状态，锁外把待写数据交给存储队列；不做任何 IO。

#include <atomic>
#include <cstdint>

#include "monitor.grpc.pb.h"
#include "node_table.h"
#include "storage/storage_queue.h"

namespace lm::manager {

class CollectorService final : public lm::Collector::Service {
 public:
  CollectorService(NodeTable& table, BoundedQueue<AcceptedSample>& queue)
      : table_(table), queue_(queue) {}

  grpc::Status Report(grpc::ServerContext* ctx, const lm::Sample* req, lm::Ack* resp) override;
  grpc::Status OnTaskEvent(grpc::ServerContext* ctx, const lm::TaskEvent* req,
                           lm::Ack* resp) override;

  uint64_t accepted() const { return accepted_.load(); }
  uint64_t stale() const { return stale_.load(); }

 private:
  NodeTable& table_;
  BoundedQueue<AcceptedSample>& queue_;
  std::atomic<uint64_t> accepted_{0};
  std::atomic<uint64_t> stale_{0};
};

}  // namespace lm::manager
