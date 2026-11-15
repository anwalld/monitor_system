#pragma once

// Advisor gRPC 服务（M9）：AI 服务推来每台节点 30 秒后的 CPU 预测与调整因子。
// 只在节点表锁内做赋值，不做任何 IO；未知节点忽略并计数，整批仍返回 OK。

#include <atomic>
#include <cstdint>

#include "advisor.grpc.pb.h"
#include "ingest/node_table.h"

namespace lm::manager {

class AdvisorService final : public lm::Advisor::Service {
 public:
  explicit AdvisorService(NodeTable& table) : table_(table) {}

  grpc::Status PushPredictions(grpc::ServerContext* ctx, const lm::PredictionBatch* req,
                               lm::Ack* resp) override;

  uint64_t batches() const { return batches_.load(); }
  uint64_t applied() const { return applied_.load(); }
  uint64_t unknown_nodes() const { return unknown_.load(); }

 private:
  NodeTable& table_;
  std::atomic<uint64_t> batches_{0};
  std::atomic<uint64_t> applied_{0};
  std::atomic<uint64_t> unknown_{0};
};

}  // namespace lm::manager
