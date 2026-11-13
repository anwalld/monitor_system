#include "prober.h"

#include <chrono>
#include <vector>

#include "lm/clock.h"

namespace lm::manager {

void Prober::start() {
  if (running_.exchange(true)) return;
  thread_ = std::thread([this] { run(); });
}

void Prober::stop() {
  if (!running_.exchange(false)) return;
  if (thread_.joinable()) thread_.join();
}

lm::WorkerTask::Stub* Prober::stub_for(const std::string& addr) {
  auto& s = stubs_[addr];
  if (!s) {
    grpc::ChannelArguments args;
    args.SetInt(GRPC_ARG_INITIAL_RECONNECT_BACKOFF_MS, 500);
    args.SetInt(GRPC_ARG_MIN_RECONNECT_BACKOFF_MS, 500);
    args.SetInt(GRPC_ARG_MAX_RECONNECT_BACKOFF_MS, 1000);
    s = lm::WorkerTask::NewStub(
        grpc::CreateCustomChannel(addr, grpc::InsecureChannelCredentials(), args));
  }
  return s.get();
}

void Prober::probe_round() {
  struct Call {
    std::string node_id;
    uint64_t seq = 0;
    grpc::ClientContext ctx;
    lm::ProbeReq req;
    lm::ProbeResp resp;
    grpc::Status status;
    std::unique_ptr<grpc::ClientAsyncResponseReader<lm::ProbeResp>> reader;
  };
  const auto targets = table_.probe_targets(lm::mono_ms());
  if (targets.empty()) return;
  grpc::CompletionQueue cq;
  std::vector<std::unique_ptr<Call>> calls;
  const auto deadline = std::chrono::system_clock::now() + std::chrono::milliseconds(timeout_ms_);
  for (const auto& [id, addr] : targets) {
    auto c = std::make_unique<Call>();
    c->node_id = id;
    c->seq = ++seq_;
    c->req.set_seq(c->seq);
    c->ctx.set_deadline(deadline);
    c->reader = stub_for(addr)->AsyncProbe(&c->ctx, c->req, &cq);
    c->reader->Finish(&c->resp, &c->status, c.get());
    calls.push_back(std::move(c));
  }
  for (std::size_t i = 0; i < calls.size(); ++i) {
    void* tag = nullptr;
    bool ok = false;
    if (!cq.Next(&tag, &ok)) break;
    auto* c = static_cast<Call*>(tag);
    const bool good = ok && c->status.ok() && c->resp.seq() == c->seq;
    (good ? ok_ : failed_).fetch_add(1, std::memory_order_relaxed);
    if (auto ch = table_.on_probe(c->node_id, good, lm::mono_ms(), lm::wall_ms()); ch && sink_) {
      sink_(*ch);
    }
  }
  cq.Shutdown();
  void* tag = nullptr;
  bool ok = false;
  while (cq.Next(&tag, &ok)) {
  }
}

void Prober::run() {
  auto next = std::chrono::steady_clock::now();
  while (running_.load(std::memory_order_relaxed)) {
    probe_round();
    next += std::chrono::milliseconds(interval_ms_);
    const auto now = std::chrono::steady_clock::now();
    if (next < now) next = now;
    // 分段睡眠，便于及时退出
    while (running_.load(std::memory_order_relaxed) && std::chrono::steady_clock::now() < next) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
  }
}

}  // namespace lm::manager
