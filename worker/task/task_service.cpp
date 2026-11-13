#include "task_service.h"

#include <chrono>

#include <spdlog/spdlog.h>

#ifdef __linux__
#include <sys/prctl.h>
#include <cerrno>
#include <cstring>
#endif

#include "task_runner.h"

namespace lm::worker {

namespace {

constexpr std::size_t kMaxTaskIdLen = 128;

int64_t wall_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

}  // namespace

TaskConfig TaskConfig::parse(const lm::Config& cfg, unsigned ncpu, const char* env_token) {
  TaskConfig c;
  c.enabled = cfg.get<bool>("task.enabled", false);
  c.listen = cfg.get<std::string>("task.listen", c.listen);
  c.token = cfg.get<std::string>("task.token", "");
  if (env_token != nullptr && env_token[0] != '\0') c.token = env_token;
  c.max_concurrent = cfg.get<int>("task.max_concurrent", 0);
  c.default_timeout_s = cfg.get<uint32_t>("task.default_timeout_s", c.default_timeout_s);
  c.max_timeout_s = cfg.get<uint32_t>("task.max_timeout_s", c.max_timeout_s);
  c.output_tail_bytes = cfg.get<std::size_t>("task.output_tail_bytes", c.output_tail_bytes);
  c.rescue_max_s = cfg.get<uint32_t>("task.rescue_max_s", c.rescue_max_s);
  const YAML::Node list = cfg.node("task.allowed_commands");
  if (list.IsDefined() && !list.IsNull()) {
    if (!list.IsSequence()) throw lm::ConfigError("配置项 task.allowed_commands 必须是列表");
    for (const auto& n : list) {
      const auto cmd = n.as<std::string>();
      if (cmd.empty() || cmd.front() != '/') {
        throw lm::ConfigError("配置项 task.allowed_commands 只能写绝对路径：" + cmd);
      }
      c.allowed_commands.push_back(cmd);
    }
  }
  if (c.max_concurrent < 0) throw lm::ConfigError("配置项 task.max_concurrent 不能为负");
  if (c.max_concurrent == 0) c.max_concurrent = static_cast<int>(2 * (ncpu == 0 ? 1 : ncpu));
  if (c.rescue_max_s == 0) throw lm::ConfigError("配置项 task.rescue_max_s 必须大于 0");
  if (c.default_timeout_s == 0) throw lm::ConfigError("配置项 task.default_timeout_s 必须大于 0");
  if (c.max_timeout_s < c.default_timeout_s) {
    throw lm::ConfigError("配置项 task.max_timeout_s 不能小于 task.default_timeout_s");
  }
  return c;
}

TaskService::TaskService(TaskConfig cfg, EventSender& events, RescueSampler* sampler)
    : cfg_(std::move(cfg)),
      allowed_(cfg_.allowed_commands.begin(), cfg_.allowed_commands.end()),
      events_(events),
      sampler_(sampler) {}

void TaskService::emit(const std::string& task_id, lm::TaskEvent::Kind kind, uint32_t exec_ms) {
  lm::TaskEvent e;
  e.set_task_id(task_id);
  e.set_kind(kind);
  e.set_ts_ms(wall_ms());
  e.set_exec_ms(exec_ms);
  events_.enqueue(std::move(e));
}

grpc::Status TaskService::Submit(grpc::ServerContext* ctx, const lm::TaskReq* req,
                                 lm::TaskResult* resp) {
  if (req->task_id().empty() || req->task_id().size() > kMaxTaskIdLen) {
    rejected_.fetch_add(1, std::memory_order_relaxed);
    return {grpc::StatusCode::INVALID_ARGUMENT, "task_id 为空或超过 128 字节"};
  }
  if (!cfg_.token.empty()) {
    const auto& md = ctx->client_metadata();
    auto it = md.find("authorization");
    if (it == md.end() || std::string(it->second.data(), it->second.size()) != "Bearer " + cfg_.token) {
      rejected_.fetch_add(1, std::memory_order_relaxed);
      return {grpc::StatusCode::UNAUTHENTICATED, "token 不正确"};
    }
  }
  if (allowed_.count(req->command()) == 0) {
    rejected_.fetch_add(1, std::memory_order_relaxed);
    return {grpc::StatusCode::PERMISSION_DENIED, "命令不在白名单中：" + req->command()};
  }
  if (req->timeout_s() > cfg_.max_timeout_s) {
    rejected_.fetch_add(1, std::memory_order_relaxed);
    return {grpc::StatusCode::INVALID_ARGUMENT,
            "timeout_s 超过上限 " + std::to_string(cfg_.max_timeout_s)};
  }
  if (running_.fetch_add(1) >= cfg_.max_concurrent) {
    running_.fetch_sub(1);
    rejected_.fetch_add(1, std::memory_order_relaxed);
    emit(req->task_id(), lm::TaskEvent::FAILED, 0);  // 让 Manager 立即销账
    return {grpc::StatusCode::RESOURCE_EXHAUSTED,
            "并发任务已达上限 " + std::to_string(cfg_.max_concurrent)};
  }

  emit(req->task_id(), lm::TaskEvent::RECEIVED, 0);
  RunSpec spec;
  spec.command = req->command();
  spec.args.assign(req->args().begin(), req->args().end());
  spec.env = {"PATH=/usr/local/bin:/usr/bin:/bin", "LM_TASK_ID=" + req->task_id()};
  spec.timeout_ms = static_cast<int64_t>(req->timeout_s() != 0 ? req->timeout_s()
                                                               : cfg_.default_timeout_s) * 1000;
  spec.tail_bytes = cfg_.output_tail_bytes;
  const RunResult r = run_task(spec, [ctx] { return ctx->IsCancelled(); });
  running_.fetch_sub(1);

  const bool ok = r.exit_code == 0;
  (ok ? done_ : failed_).fetch_add(1, std::memory_order_relaxed);
  emit(req->task_id(), ok ? lm::TaskEvent::DONE : lm::TaskEvent::FAILED, r.exec_ms);
  spdlog::debug("任务 {} {} 退出码 {} 耗时 {} ms", req->task_id(), req->command(), r.exit_code,
                r.exec_ms);
  if (r.cancelled) return {grpc::StatusCode::CANCELLED, "客户端取消，已杀掉任务进程"};
  resp->set_task_id(req->task_id());
  resp->set_exit_code(r.exit_code);
  resp->set_stdout_tail(r.output_tail);
  resp->set_exec_ms(r.exec_ms);
  return grpc::Status::OK;
}

grpc::Status TaskService::Probe(grpc::ServerContext* /*ctx*/, const lm::ProbeReq* req,
                                lm::ProbeResp* resp) {
  resp->set_seq(req->seq());
  return grpc::Status::OK;
}

grpc::Status TaskService::Rescue(grpc::ServerContext* ctx,
                                 grpc::ServerReaderWriter<lm::RescueResp, lm::RescueReq>* stream) {
  if (sampler_ == nullptr) return {grpc::StatusCode::UNIMPLEMENTED, "本 Worker 未启用急救"};
  std::unique_lock<std::mutex> lk(rescue_mu_, std::try_to_lock);
  if (!lk.owns_lock()) return {grpc::StatusCode::ABORTED, "已有一条急救流"};
  rescue_active_.store(true);
  rescues_.fetch_add(1, std::memory_order_relaxed);
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(cfg_.rescue_max_s);
  uint64_t n = 0;
  grpc::Status result = grpc::Status::OK;
  lm::RescueReq req;
  while (stream->Read(&req)) {
    if (n == 0) sampler_->begin();  // 第一条请求到达时切到 1 ms 刷新
    ++n;
    if (std::chrono::steady_clock::now() >= deadline) {
      result = {grpc::StatusCode::DEADLINE_EXCEEDED,
                "急救超过 " + std::to_string(cfg_.rescue_max_s) + " 秒上限"};
      break;
    }
    lm::RescueResp resp;
    resp.set_seq(req.seq());
    resp.set_send_ts_us(req.send_ts_us());
    *resp.mutable_sample() = sampler_->sample();
    if (!stream->Write(resp)) break;
    if (ctx->IsCancelled()) break;
  }
  if (sampler_->active()) sampler_->end();
  rescue_active_.store(false);
  spdlog::info("急救流结束：收到 {} 条请求（{}）", n, result.ok() ? "正常" : result.error_message());
  return result;
}

bool clear_ambient_caps(std::string* err) {
#ifdef __linux__
  if (::prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_CLEAR_ALL, 0, 0, 0) != 0) {
    if (err != nullptr) *err = std::strerror(errno);
    return false;
  }
#else
  (void)err;
#endif
  return true;
}

}  // namespace lm::worker
