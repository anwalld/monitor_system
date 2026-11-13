#include "scheduler_loop.h"

#include <spdlog/spdlog.h>

#include "lm/clock.h"

namespace lm::manager {

namespace {

constexpr int kMaxDetailPerSecond = 10;

const char* kind_name(InflightLedger::Expired::Kind k) {
  switch (k) {
    case InflightLedger::Expired::Kind::kVoided:
      return "确认超时（未 MarkSent，作废）";
    case InflightLedger::Expired::Kind::kUnconfirmed:
      return "确认超时（已发送未收到，计入未确认）";
    case InflightLedger::Expired::Kind::kExecTimeout:
      return "执行超时";
  }
  return "?";
}

}  // namespace

void SchedulerLoop::start() {
  if (running_.exchange(true)) return;
  thread_ = std::thread([this] { run(); });
}

void SchedulerLoop::stop() {
  if (!running_.exchange(false)) return;
  if (thread_.joinable()) thread_.join();
}

void SchedulerLoop::tick_once(int64_t now_mono_ms) {
  expired_.clear();
  table_.tick(now_mono_ms, &expired_);
  if (last_second_ms_ < 0) last_second_ms_ = now_mono_ms;
  const int64_t sec_ms = opt_.second.count();
  if (now_mono_ms - last_second_ms_ >= sec_ms) {
    table_.tick_second(static_cast<double>(now_mono_ms - last_second_ms_) / 1000.0);
    last_second_ms_ = now_mono_ms;
    detail_budget_ = kMaxDetailPerSecond;
  }
  // 锁外打日志
  uint64_t skipped = 0;
  for (const auto& e : expired_) {
    switch (e.kind) {
      case InflightLedger::Expired::Kind::kVoided:
        voided_.fetch_add(1, std::memory_order_relaxed);
        break;
      case InflightLedger::Expired::Kind::kUnconfirmed:
        unconfirmed_.fetch_add(1, std::memory_order_relaxed);
        break;
      case InflightLedger::Expired::Kind::kExecTimeout:
        exec_timeouts_.fetch_add(1, std::memory_order_relaxed);
        break;
    }
    if (detail_budget_ > 0) {
      --detail_budget_;
      spdlog::warn("任务 {}（节点 {}）{}", e.task_id, e.node_id, kind_name(e.kind));
    } else {
      ++skipped;
    }
  }
  if (skipped > 0) spdlog::warn("另有 {} 个任务超时，日志已省略", skipped);
}

void SchedulerLoop::run() {
  auto next = std::chrono::steady_clock::now();
  while (running_.load(std::memory_order_relaxed)) {
    tick_once(lm::mono_ms());
    next += opt_.tick;
    const auto now = std::chrono::steady_clock::now();
    if (next < now) next = now;  // 迟到不补拍
    std::this_thread::sleep_until(next);
  }
}

}  // namespace lm::manager
