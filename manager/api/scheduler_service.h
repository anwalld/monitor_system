#pragma once

// Scheduler gRPC 服务：客户端申请节点（Allocate，选中即记账）、标记已发送（MarkSent）。
// 只在节点表锁内做内存操作，不做任何 IO。

#include <atomic>
#include <cstdint>
#include <mutex>
#include <random>
#include <string>

#include "ingest/node_table.h"
#include "task.grpc.pb.h"

namespace lm::manager {

class SchedulerService final : public lm::Scheduler::Service {
 public:
  struct Options {
    int64_t start_wall_ms = 0;  // task_id 前缀；0 表示取构造时刻
    uint64_t seed = 0;          // 随机数种子；0 表示用 random_device
    std::size_t max_client_id_len = 64;
  };

  SchedulerService(NodeTable& table, Options opt);

  grpc::Status Allocate(grpc::ServerContext* ctx, const lm::AllocateReq* req,
                        lm::AllocateResp* resp) override;
  grpc::Status MarkSent(grpc::ServerContext* ctx, const lm::TaskRef* req, lm::Ack* resp) override;

  uint64_t allocated() const { return allocated_.load(); }
  uint64_t rejected() const { return rejected_.load(); }
  const std::string& task_prefix() const { return prefix_; }

 private:
  double uniform();

  NodeTable& table_;
  const Options opt_;
  std::string prefix_;
  std::atomic<uint64_t> next_seq_{1};
  std::mutex rng_mu_;
  std::mt19937_64 rng_;
  std::atomic<uint64_t> allocated_{0};
  std::atomic<uint64_t> rejected_{0};
};

}  // namespace lm::manager
