#include "scheduler_service.h"

#include <cstdio>

#include <spdlog/spdlog.h>

#include "lm/clock.h"

namespace lm::manager {

SchedulerService::SchedulerService(NodeTable& table, Options opt)
    : table_(table), opt_(opt) {
  const int64_t start = opt_.start_wall_ms != 0 ? opt_.start_wall_ms : lm::wall_ms();
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%llx", static_cast<unsigned long long>(start));
  prefix_ = buf;
  rng_.seed(opt_.seed != 0 ? opt_.seed : std::random_device{}());
}

double SchedulerService::uniform() {
  std::lock_guard<std::mutex> lk(rng_mu_);
  return std::uniform_real_distribution<double>(0.0, 1.0)(rng_);
}

grpc::Status SchedulerService::Allocate(grpc::ServerContext* /*ctx*/, const lm::AllocateReq* req,
                                        lm::AllocateResp* resp) {
  if (req->client_id().empty()) {
    return {grpc::StatusCode::INVALID_ARGUMENT, "client_id 不能为空"};
  }
  if (req->client_id().size() > opt_.max_client_id_len) {
    return {grpc::StatusCode::INVALID_ARGUMENT,
            "client_id 超过 " + std::to_string(opt_.max_client_id_len) + " 字节"};
  }
  const double u = uniform();  // 锁外生成随机数和 task_id
  const uint64_t seq = next_seq_.fetch_add(1, std::memory_order_relaxed);
  std::string task_id = prefix_ + "-" + std::to_string(seq);
  const SelectResult r = table_.allocate(lm::mono_ms(), u, task_id, req->input_bytes());
  if (!r.pick) {
    rejected_.fetch_add(1, std::memory_order_relaxed);
    return {grpc::StatusCode::RESOURCE_EXHAUSTED,
            "没有可用节点：总数 " + std::to_string(r.total) + "，剔除 " + std::to_string(r.evicted) +
                "，失联 " + std::to_string(r.lost)};
  }
  resp->set_task_id(std::move(task_id));
  resp->set_worker_addr(r.pick->addr);
  allocated_.fetch_add(1, std::memory_order_relaxed);
  spdlog::debug("分配 {} → {}（{}），概率 {:.3f}，客户端 {}，输入 {} 字节，类型 {}", resp->task_id(),
                r.pick->node_id, r.pick->addr, r.pick->prob, req->client_id(), req->input_bytes(),
                req->kind());
  return grpc::Status::OK;
}

grpc::Status SchedulerService::MarkSent(grpc::ServerContext* /*ctx*/, const lm::TaskRef* req,
                                        lm::Ack* /*resp*/) {
  if (req->task_id().empty()) return {grpc::StatusCode::INVALID_ARGUMENT, "task_id 不能为空"};
  if (table_.mark_sent(req->task_id(), lm::mono_ms()) == InflightLedger::MarkResult::kNotFound) {
    return {grpc::StatusCode::NOT_FOUND, "未知任务 " + req->task_id()};
  }
  return grpc::Status::OK;
}

}  // namespace lm::manager
