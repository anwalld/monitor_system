#include "advisor_service.h"

#include <spdlog/spdlog.h>

#include "lm/clock.h"

namespace lm::manager {

grpc::Status AdvisorService::PushPredictions(grpc::ServerContext* /*ctx*/,
                                             const lm::PredictionBatch* req, lm::Ack* /*resp*/) {
  std::vector<PredictionItem> items;
  items.reserve(static_cast<std::size_t>(req->items_size()));
  for (const auto& p : req->items()) {
    if (p.node_id().empty()) return {grpc::StatusCode::INVALID_ARGUMENT, "node_id 不能为空"};
    items.push_back({p.node_id(), p.cpu_busy_30s(), p.factor()});
  }
  const PredictionResult r = table_.set_predictions(items, req->ttl_ms(), lm::mono_ms());
  batches_.fetch_add(1, std::memory_order_relaxed);
  applied_.fetch_add(static_cast<uint64_t>(r.applied), std::memory_order_relaxed);
  unknown_.fetch_add(static_cast<uint64_t>(r.unknown), std::memory_order_relaxed);
  if (r.unknown > 0) {
    spdlog::debug("预测批含 {} 个未知节点（已忽略），生效 {} 个，ttl {} ms", r.unknown, r.applied, r.ttl_ms);
  }
  return grpc::Status::OK;
}

}  // namespace lm::manager
