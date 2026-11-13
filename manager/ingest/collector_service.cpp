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
      if (acc.hint_runq_missing) {
        spdlog::info("节点 {} 未上报调度延迟，该项按 {} 计分，建议升级 Worker", req->node_id(),
                     table_.options().scoring.missing_score);
      }
      if (acc.hint_net_missing) {
        spdlog::info("节点 {} 没有网卡链路数据，网络分按 {} 计", req->node_id(),
                     table_.options().scoring.missing_score);
      }
      if (acc.evicted_changed) {
        if (acc.evicted) {
          spdlog::warn("节点 {} 关键指标越限，剔除（{}），分数 {:.3f}", req->node_id(),
                       acc.evict_reason, acc.score.total);
        } else {
          spdlog::info("节点 {} 关键指标恢复，回到候选集合，分数 {:.3f}", req->node_id(),
                       acc.score.total);
        }
      }
      if (acc.change && acc.change->from != acc.change->to) {
        const auto& c = *acc.change;
        if (c.to == NodeMode::kEvicted) {
          spdlog::warn("节点 {} 状态 {} → {}（{}）", c.node_id, mode_name(c.from), mode_name(c.to),
                       c.reason);
        } else {
          spdlog::info("节点 {} 状态 {} → {}", c.node_id, mode_name(c.from), mode_name(c.to));
        }
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
  if (req->task_id().empty()) return {grpc::StatusCode::INVALID_ARGUMENT, "task_id 不能为空"};
  const auto r = table_.on_event(req->task_id(), req->kind(), lm::mono_ms());
  if (r == InflightLedger::EventResult::kLate) {
    // 迟到或未知的事件只计数（M7 用它更新 task 表），Worker 不需要知道
    spdlog::debug("迟到的任务事件 {} {}", req->task_id(), lm::TaskEvent::Kind_Name(req->kind()));
  }
  return grpc::Status::OK;
}

}  // namespace lm::manager
