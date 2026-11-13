#include "reporter.h"

#include <spdlog/spdlog.h>

namespace lm::worker {

Reporter::Reporter(SampleRing& ring, std::shared_ptr<grpc::Channel> channel, Options opt)
    : ring_(ring), stub_(lm::Collector::NewStub(std::move(channel))), opt_(opt) {}

Reporter::Reporter(SampleRing& ring, Sink sink, Options opt)
    : ring_(ring), sink_(std::move(sink)), opt_(opt) {}

Reporter::~Reporter() { stop(); }

void Reporter::start() {
  if (running_.exchange(true)) return;
  thread_ = std::thread([this] { run(); });
}

void Reporter::stop() {
  running_.store(false);
  if (thread_.joinable()) thread_.join();
}

void Reporter::run() {
  const auto phase = opt_.phase.count() < 0 ? opt_.interval / 2 : opt_.phase;
  auto next = std::chrono::steady_clock::now() + phase;
  while (running_.load(std::memory_order_relaxed)) {
    while (running_.load(std::memory_order_relaxed)) {
      const auto left = next - std::chrono::steady_clock::now();
      if (left <= std::chrono::milliseconds::zero()) break;
      std::this_thread::sleep_for(std::min<std::chrono::steady_clock::duration>(
          left, std::chrono::milliseconds(100)));
    }
    if (!running_.load(std::memory_order_relaxed)) break;
    report_once();
    next += opt_.interval;
    const auto now = std::chrono::steady_clock::now();
    if (next < now) next = now + opt_.interval;
  }
}

bool Reporter::report_once() {
  lm::Sample s;
  if (!ring_.take_latest(s)) return false;
  if (s.seq() <= last_seq_.load(std::memory_order_relaxed)) return false;  // 已发过
  last_seq_.store(s.seq(), std::memory_order_relaxed);

  if (sink_) {
    sink_(s);
    ok_.fetch_add(1);
    return true;
  }

  grpc::ClientContext ctx;
  ctx.set_deadline(std::chrono::system_clock::now() + opt_.deadline);
  lm::Ack ack;
  const grpc::Status st = stub_->Report(&ctx, s, &ack);
  if (st.ok()) {
    if (failed_.load() > 0 && ok_.load() == 0) spdlog::info("上报恢复");
    ok_.fetch_add(1);
    return true;
  }
  failed_.fetch_add(1);
  uint64_t suppressed = 0;
  if (fail_log_.allow(&suppressed)) {
    spdlog::warn("上报失败 seq={} code={} msg={}（此前 10 秒内另有 {} 次失败未记录）", s.seq(),
                 static_cast<int>(st.error_code()), st.error_message(), suppressed);
  }
  return true;
}

}  // namespace lm::worker
