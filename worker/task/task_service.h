#pragma once

// WorkerTask gRPC 服务（M5）：Submit 执行白名单里的命令并上报 RECEIVED、DONE/FAILED；Probe 回 seq；
// Rescue 在 M6 实现（基类默认返回 UNIMPLEMENTED）。

#include <atomic>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <vector>

#include "event_sender.h"
#include "lm/config.h"
#include "task.grpc.pb.h"

namespace lm::worker {

struct TaskConfig {
  bool enabled = false;
  std::string listen = "0.0.0.0:50052";
  std::vector<std::string> allowed_commands;
  std::string token;
  int max_concurrent = 0;  // 解析后为实际值（0 → 2 × 核数）
  uint32_t default_timeout_s = 300;
  uint32_t max_timeout_s = 3600;
  std::size_t output_tail_bytes = 4096;

  // 解析 task 段；环境变量 LM_TASK_TOKEN 非空时覆盖 token。不合法抛 lm::ConfigError。
  static TaskConfig parse(const lm::Config& cfg, unsigned ncpu, const char* env_token);
};

class TaskService final : public lm::WorkerTask::Service {
 public:
  TaskService(TaskConfig cfg, EventSender& events);

  grpc::Status Submit(grpc::ServerContext* ctx, const lm::TaskReq* req,
                      lm::TaskResult* resp) override;
  grpc::Status Probe(grpc::ServerContext* ctx, const lm::ProbeReq* req,
                     lm::ProbeResp* resp) override;

  int running() const { return running_.load(); }
  uint64_t done() const { return done_.load(); }
  uint64_t failed() const { return failed_.load(); }
  uint64_t rejected() const { return rejected_.load(); }

 private:
  void emit(const std::string& task_id, lm::TaskEvent::Kind kind, uint32_t exec_ms);

  const TaskConfig cfg_;
  const std::unordered_set<std::string> allowed_;
  EventSender& events_;
  std::atomic<int> running_{0};
  std::atomic<uint64_t> done_{0};
  std::atomic<uint64_t> failed_{0};
  std::atomic<uint64_t> rejected_{0};
};

// 清空本进程的 ambient capability，之后 exec 的子进程不再继承能力（Linux）。其他平台返回 true。
bool clear_ambient_caps(std::string* err);

}  // namespace lm::worker
