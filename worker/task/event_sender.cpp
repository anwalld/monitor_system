#include "event_sender.h"

#include <spdlog/spdlog.h>

namespace lm::worker {

EventSender::EventSender(std::shared_ptr<grpc::Channel> channel, Options opt)
    : stub_(lm::Collector::NewStub(channel)), opt_(opt) {
  send_ = [this](const lm::TaskEvent& e, std::chrono::milliseconds deadline) {
    grpc::ClientContext ctx;
    ctx.set_deadline(std::chrono::system_clock::now() + deadline);
    lm::Ack ack;
    return stub_->OnTaskEvent(&ctx, e, &ack);
  };
}

EventSender::EventSender(SendFn send, Options opt) : send_(std::move(send)), opt_(opt) {}

EventSender::~EventSender() { stop(); }

void EventSender::start() {
  std::lock_guard<std::mutex> lk(mu_);
  if (started_) return;
  started_ = true;
  stopping_ = false;
  thread_ = std::thread([this] { run(); });
}

void EventSender::stop() {
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (!started_) return;
    stopping_ = true;
  }
  cv_.notify_all();
  if (thread_.joinable()) thread_.join();
  std::lock_guard<std::mutex> lk(mu_);
  started_ = false;
}

void EventSender::enqueue(lm::TaskEvent e) {
  {
    std::lock_guard<std::mutex> lk(mu_);
    if (q_.size() >= opt_.capacity) {
      q_.pop_front();
      dropped_full_.fetch_add(1, std::memory_order_relaxed);
    }
    q_.push_back(std::move(e));
  }
  cv_.notify_one();
}

std::size_t EventSender::queued() const {
  std::lock_guard<std::mutex> lk(mu_);
  return q_.size();
}

bool EventSender::send_with_retry(const lm::TaskEvent& e, bool retry) {
  const int attempts = retry ? opt_.max_attempts : 1;
  grpc::Status st;
  for (int i = 0; i < attempts; ++i) {
    if (i > 0) {
      std::unique_lock<std::mutex> lk(mu_);
      // 退避期间收到停止信号就不再等
      if (cv_.wait_for(lk, opt_.backoff * (1 << (i - 1)), [this] { return stopping_; })) break;
    }
    st = send_(e, opt_.deadline);
    if (st.ok()) {
      sent_.fetch_add(1, std::memory_order_relaxed);
      return true;
    }
  }
  dropped_failed_.fetch_add(1, std::memory_order_relaxed);
  uint64_t suppressed = 0;
  if (fail_log_.allow(&suppressed)) {
    spdlog::warn("任务事件 {} {} 发送失败，已丢弃: {}（此前 10 秒内另有 {} 次未记录）", e.task_id(),
                 lm::TaskEvent::Kind_Name(e.kind()), st.error_message(), suppressed);
  }
  return false;
}

void EventSender::run() {
  for (;;) {
    lm::TaskEvent e;
    bool stopping = false;
    {
      std::unique_lock<std::mutex> lk(mu_);
      cv_.wait(lk, [this] { return stopping_ || !q_.empty(); });
      if (q_.empty()) return;  // 停止且已发完
      e = std::move(q_.front());
      q_.pop_front();
      stopping = stopping_;
    }
    send_with_retry(e, !stopping);
  }
}

}  // namespace lm::worker
