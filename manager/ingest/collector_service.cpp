#include "collector_service.h"

#include <spdlog/spdlog.h>

#include "lm/clock.h"

namespace lm::manager {

grpc::Status CollectorService::Report(grpc::ServerContext* ctx, const lm::Sample* req,
                                      lm::Ack* /*resp*/) {
  if (auto why = table_.validate(*req)) {
    return {grpc::StatusCode::INVALID_ARGUMENT, *why};
  }
  AcceptedSample acc;
  const auto r = table_.apply(*req, parse_peer_ip(ctx->peer()), lm::mono_ms(), lm::wall_ms(), &acc);
  switch (r) {
    case NodeTable::Result::kAccepted:
      accepted_.fetch_add(1, std::memory_order_relaxed);
      if (acc.new_node) {
        spdlog::info("新节点 {} 地址 {} 核数 {}", req->node_id(), acc.addr, acc.ncpu);
      }
      queue_.push(std::move(acc));  // 锁外入队
      break;
    case NodeTable::Result::kStale:
      stale_.fetch_add(1, std::memory_order_relaxed);
      break;
    case NodeTable::Result::kInvalid:
      return {grpc::StatusCode::INVALID_ARGUMENT, "invalid sample"};
  }
  return grpc::Status::OK;
}

grpc::Status CollectorService::OnTaskEvent(grpc::ServerContext* /*ctx*/, const lm::TaskEvent* req,
                                           lm::Ack* /*resp*/) {
  if (req->kind() == lm::TaskEvent::KIND_UNSPECIFIED) {
    return {grpc::StatusCode::INVALID_ARGUMENT, "kind 不能为 KIND_UNSPECIFIED"};
  }
  // M5 实现在途任务账；M1 只确认收到
  return grpc::Status::OK;
}

}  // namespace lm::manager
