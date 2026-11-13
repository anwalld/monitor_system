#include "rescue_session.h"

#include <chrono>

#include <spdlog/spdlog.h>

#include "lm/clock.h"

namespace lm::manager {

namespace {

int64_t mono_us() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace

RescueSession::RescueSession(NodeTable& table, RescueStart start, RescueParams p, StateSink sink)
    : table_(table), start_(std::move(start)), p_(p), sink_(std::move(sink)),
      tracker_(p.timeout_ms * 1000) {}

RescueSession::~RescueSession() {
  cancel();
  join();
}

void RescueSession::start() { thread_ = std::thread([this] { run(); }); }

void RescueSession::join() {
  if (thread_.joinable()) thread_.join();
}

RescueStats RescueSession::stats() const {
  std::lock_guard<std::mutex> lk(mu_);
  return tracker_.stats();
}

void RescueSession::run() {
  // 单独的 channel：本地子通道池，不与探测、上报共用连接
  grpc::ChannelArguments args;
  args.SetInt(GRPC_ARG_USE_LOCAL_SUBCHANNEL_POOL, 1);
  auto stub = lm::WorkerTask::NewStub(
      grpc::CreateCustomChannel(start_.addr, grpc::InsecureChannelCredentials(), args));
  grpc::ClientContext ctx;
  ctx.set_deadline(std::chrono::system_clock::now() + std::chrono::milliseconds(p_.max_ms + 1000));
  auto stream = stub->Rescue(&ctx);

  std::thread reader([&] {
    lm::RescueResp resp;
    while (stream->Read(&resp)) {
      std::lock_guard<std::mutex> lk(mu_);
      tracker_.on_response(resp.seq(), mono_us());
    }
  });

  RescueTarget target(p_.epsilon_factor);
  int n = start_.candidates;
  double tgt = RescueTarget::target_for(0, n, p_.epsilon_factor);
  RescueProbe last;
  const auto t0 = std::chrono::steady_clock::now();
  auto next_send = t0;
  auto next_batch = t0 + std::chrono::milliseconds(p_.batch_ms);
  const auto end_by = t0 + std::chrono::milliseconds(p_.max_ms);
  uint64_t seq = 0;
  bool writable = true;
  const char* why = "满时长";
  while (!cancel_.load(std::memory_order_relaxed)) {
    const auto now = std::chrono::steady_clock::now();
    {
      std::lock_guard<std::mutex> lk(mu_);
      tracker_.check(mono_us());
    }
    if (now >= next_send) {
      lm::RescueReq req;
      req.set_seq(++seq);
      req.set_send_ts_us(mono_us());
      {
        std::lock_guard<std::mutex> lk(mu_);
        tracker_.on_send(seq, req.send_ts_us());
      }
      // 流断了就不再写，但继续计时：这些请求都会按超时结算
      if (writable) writable = stream->Write(req);
      next_send += std::chrono::milliseconds(p_.interval_ms);
      if (next_send < now) next_send = now;  // 落后不补发
    }
    if (now >= next_batch) {
      bool ok;
      {
        std::lock_guard<std::mutex> lk(mu_);
        ok = tracker_.settle();
      }
      tgt = target.on_batch(ok, n);
      last = table_.rescue_batch(start_.node_id, ok, lm::mono_ms());
      if (!last.found) {
        why = "节点不存在";
        break;
      }
      n = last.candidates;
      tgt = RescueTarget::target_for(target.k(), n, p_.epsilon_factor);
      if (RescueTarget::reached(last.prob, tgt)) {
        why = "达到目标";
        break;
      }
      if (now >= end_by) break;
      next_batch += std::chrono::milliseconds(p_.batch_ms);
    }
    std::this_thread::sleep_until(std::min(next_send, next_batch));
  }
  if (cancel_.load()) why = "被取消";
  if (writable) stream->WritesDone();
  ctx.TryCancel();
  reader.join();
  (void)stream->Finish();

  const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - t0).count();
  const RescueStats st = stats();
  auto change = table_.end_rescue(start_.node_id, lm::mono_ms(), lm::wall_ms());
  spdlog::info(
      "急救结束 {}（{}）：{} ms，批 成功 {} / 失败 {}，请求 {}，成功 {}，超时 {}，迟到 {}，网络事件 {}，"
      "概率 {:.3f} / 目标 {:.3f}，T {:.3f} → {:.3f}，转为 {}",
      start_.node_id, why, elapsed_ms, st.batches_ok, st.batches_fail, st.sent, st.ok, st.timeouts,
      st.late, st.events, last.prob, tgt, start_.timeout_count, last.timeout_count,
      change ? mode_name(change->to) : "?");
  if (change && sink_) sink_(*change);
  done_.store(true);
}

void RescueManager::launch(const RescueStart& s) {
  if (sessions_.count(s.node_id) != 0) return;
  spdlog::warn("节点 {} 开始急救：选中概率 {:.3f} < {:.3f}/{}，T {:.3f}", s.node_id, s.prob,
               p_.prob_factor, s.candidates, s.timeout_count);
  auto sess = std::make_unique<RescueSession>(table_, s, p_, sink_);
  sess->start();
  sessions_[s.node_id] = std::move(sess);
}

void RescueManager::reap() {
  for (auto it = sessions_.begin(); it != sessions_.end();) {
    if (it->second->done()) {
      it->second->join();
      it = sessions_.erase(it);
      ++finished_;
    } else {
      ++it;
    }
  }
}

void RescueManager::stop() {
  for (auto& [id, s] : sessions_) s->cancel();
  for (auto& [id, s] : sessions_) s->join();
  sessions_.clear();
}

}  // namespace lm::manager
