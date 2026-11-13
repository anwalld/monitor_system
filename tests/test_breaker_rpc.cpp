// 低频探测与急救会话（进程内 gRPC，Worker 端用真实的 TaskService）。

#include <gtest/gtest.h>

#include <grpcpp/grpcpp.h>

#include <chrono>
#include <thread>

#include "breaker/prober.h"
#include "breaker/rescue_session.h"
#include "event_sender.h"
#include "lm/clock.h"
#include "task_service.h"

using namespace lm::manager;
using lm::worker::EventSender;
using lm::worker::RescueSampler;
using lm::worker::TaskConfig;
using lm::worker::TaskService;

namespace {

lm::Sample healthy(const std::string& id, uint64_t seq) {
  lm::Sample s;
  s.set_node_id(id);
  s.set_seq(seq);
  s.add_core_busy(0.1F);
  s.set_cpu_busy(0.1F);
  s.set_mem_avail_ratio(0.8F);
  s.set_runq_p99_us(50.0F);
  return s;
}

struct FakeWorker {
  std::unique_ptr<EventSender> events;
  std::unique_ptr<RescueSampler> sampler;
  std::unique_ptr<TaskService> svc;
  std::unique_ptr<grpc::Server> server;
  int port = 0;
  FakeWorker() {
    events = std::make_unique<EventSender>(
        [](const lm::TaskEvent&, std::chrono::milliseconds) { return grpc::Status::OK; },
        EventSender::Options{});
    sampler = std::make_unique<RescueSampler>(nullptr, [] { return lm::Sample{}; });
    TaskConfig c;
    c.enabled = true;
    c.max_concurrent = 4;
    svc = std::make_unique<TaskService>(c, *events, sampler.get());
    grpc::ServerBuilder b;
    b.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    b.RegisterService(svc.get());
    server = b.BuildAndStart();
  }
};

// 不可达的 Worker 端口：本机端口 1 没有服务，连接被拒绝
int silent_port() { return 1; }

TEST(Prober, HealthyNodeRoundIsFast) {
  FakeWorker w;
  NodeTable::Options o;
  o.worker_port = static_cast<uint16_t>(w.port);
  NodeTable t(o);
  t.apply(healthy("good", 1), "127.0.0.1", lm::mono_ms(), 0, nullptr);
  std::vector<StateChange> changes;
  Prober p(t, o.breaker, [&](const StateChange& c) { changes.push_back(c); });
  const auto t0 = std::chrono::steady_clock::now();
  p.probe_round();
  EXPECT_LT(std::chrono::steady_clock::now() - t0, std::chrono::milliseconds(600));
  EXPECT_EQ(p.ok(), 1U);
  EXPECT_EQ(t.get("good")->br.probe_fail_streak, 0);
}

TEST(Prober, UnreachableNodeEvicted) {
  NodeTable::Options o;
  o.worker_port = static_cast<uint16_t>(silent_port());
  NodeTable t(o);
  std::vector<StateChange> changes;
  Prober p(t, o.breaker, [&](const StateChange& c) { changes.push_back(c); });
  uint64_t seq = 0;
  for (int i = 0; i < 3; ++i) {
    t.apply(healthy("bad", ++seq), "127.0.0.1", lm::mono_ms(), 0, nullptr);
    const auto t0 = std::chrono::steady_clock::now();
    p.probe_round();
    EXPECT_LT(std::chrono::steady_clock::now() - t0, std::chrono::milliseconds(700));
  }
  EXPECT_EQ(p.failed(), 3U);
  ASSERT_FALSE(changes.empty());
  EXPECT_EQ(changes.back().to, NodeMode::kEvicted);
  EXPECT_EQ(changes.back().reason, "probe");
}

// 让 w1 进入急救：两台节点正常，w1 有 4 次执行超时
void prepare_rescue(NodeTable& t, std::vector<RescueStart>* rescues) {
  const int64_t now = lm::mono_ms();
  t.apply(healthy("w1", 1), "127.0.0.1", now, 0, nullptr);
  t.apply(healthy("w2", 1), "127.0.0.2", now, 0, nullptr);
  t.tick_states(now + 1, 0, nullptr, nullptr);
  for (int i = 0; i < 4; ++i) {
    const std::string id = "x" + std::to_string(i);
    t.allocate(now + 1, 0.0, id, 0);
    t.on_event(id, lm::TaskEvent::RECEIVED, now + 1);
  }
  t.tick(now + 1 + 300 * 1000, nullptr);  // 模拟时钟推进：执行超时
  t.tick_states(now + 2, 0, nullptr, rescues);
}

TEST(RescueSession, HealthyWorkerRecoversToSlowStart) {
  FakeWorker w;
  NodeTable::Options o;
  o.worker_port = static_cast<uint16_t>(w.port);
  o.breaker.slow_start_ms = 1;
  NodeTable t(o);
  std::vector<RescueStart> rescues;
  prepare_rescue(t, &rescues);
  ASSERT_EQ(rescues.size(), 1U);
  ASSERT_EQ(rescues[0].node_id, "w1");
  std::vector<StateChange> changes;
  RescueManager m(t, o.breaker.rescue, [&](const StateChange& c) { changes.push_back(c); });
  m.launch(rescues[0]);
  uint64_t seq = 1;
  for (int i = 0; i < 300 && m.active() > 0; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    if (i % 10 == 0) {  // 两台节点照常每 0.5 秒上报，保持新鲜
      ++seq;
      t.apply(healthy("w1", seq), "127.0.0.1", lm::mono_ms(), 0, nullptr);
      t.apply(healthy("w2", seq), "127.0.0.2", lm::mono_ms(), 0, nullptr);
    }
    m.reap();
  }
  ASSERT_EQ(m.active(), 0U);
  ASSERT_FALSE(changes.empty());
  EXPECT_EQ(changes.back().to, NodeMode::kSlowStart);
  // 空闲时约 44 批全部成功（4 × 0.9^44 ≈ 0.039）；机器繁忙时可能有超时批、提前结束，至少要有成功批让 T 下降
  EXPECT_LT(t.inflight_snapshot("w1").timeout_count, 4.0 * 0.9 + 1e-9);
  EXPECT_EQ(w.svc->rescues(), 1U);
}

TEST(RescueSession, UnreachableWorkerEndsQuickly) {
  NodeTable::Options o;
  o.worker_port = static_cast<uint16_t>(silent_port());
  o.breaker.slow_start_ms = 1;
  NodeTable t(o);
  std::vector<RescueStart> rescues;
  prepare_rescue(t, &rescues);
  ASSERT_EQ(rescues.size(), 1U);
  RescueSession s(t, rescues[0], o.breaker.rescue, nullptr);
  const auto t0 = std::chrono::steady_clock::now();
  s.start();
  s.join();
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0).count();
  EXPECT_LT(ms, 1000);
  EXPECT_GT(s.stats().timeouts, 0U);
  // N = 2、p ≈ 0.119：连续 3 批失败后目标降到 0.0625 < p，急救结束
  EXPECT_EQ(s.stats().batches_ok, 0);
  EXPECT_GE(s.stats().batches_fail, 3);
  EXPECT_NE(*t.mode("w1"), NodeMode::kRescue);
}

}  // namespace
