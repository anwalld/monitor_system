// EventSender 与 TaskService（进程内 gRPC，假 Collector 收事件）。

#include <gtest/gtest.h>

#include <grpcpp/grpcpp.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include "event_sender.h"
#include "task_service.h"
#include "collector/cpu_source.h"

using namespace lm::worker;

namespace {

// ---------- EventSender（不经过 gRPC） ----------

lm::TaskEvent ev(const std::string& id, lm::TaskEvent::Kind k) {
  lm::TaskEvent e;
  e.set_task_id(id);
  e.set_kind(k);
  return e;
}

EventSender::Options fast() {
  EventSender::Options o;
  o.backoff = std::chrono::milliseconds(5);
  return o;
}

bool wait_for(const std::function<bool()>& f, int ms = 2000) {
  for (int i = 0; i < ms / 5; ++i) {
    if (f()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return f();
}

TEST(EventSender, DeliversInOrder) {
  std::mutex mu;
  std::vector<std::string> got;
  EventSender s(
      [&](const lm::TaskEvent& e, std::chrono::milliseconds) {
        std::lock_guard<std::mutex> lk(mu);
        got.push_back(e.task_id() + ":" + lm::TaskEvent::Kind_Name(e.kind()));
        return grpc::Status::OK;
      },
      fast());
  s.start();
  s.enqueue(ev("a", lm::TaskEvent::RECEIVED));
  s.enqueue(ev("a", lm::TaskEvent::DONE));
  ASSERT_TRUE(wait_for([&] { return s.sent() == 2; }));
  s.stop();
  EXPECT_EQ(got, (std::vector<std::string>{"a:RECEIVED", "a:DONE"}));
}

TEST(EventSender, RetriesAfterUnavailable) {
  std::atomic<int> calls{0};
  EventSender s(
      [&](const lm::TaskEvent&, std::chrono::milliseconds) {
        return calls.fetch_add(1) == 0 ? grpc::Status(grpc::StatusCode::UNAVAILABLE, "down")
                                       : grpc::Status::OK;
      },
      fast());
  s.start();
  s.enqueue(ev("a", lm::TaskEvent::DONE));
  ASSERT_TRUE(wait_for([&] { return s.sent() == 1; }));
  s.stop();
  EXPECT_EQ(calls.load(), 2);
  EXPECT_EQ(s.dropped_failed(), 0U);
}

TEST(EventSender, GivesUpAfterThreeAttempts) {
  std::atomic<int> calls{0};
  EventSender s(
      [&](const lm::TaskEvent&, std::chrono::milliseconds) {
        calls.fetch_add(1);
        return grpc::Status(grpc::StatusCode::UNAVAILABLE, "down");
      },
      fast());
  s.start();
  s.enqueue(ev("a", lm::TaskEvent::DONE));
  ASSERT_TRUE(wait_for([&] { return s.dropped_failed() == 1; }));
  s.stop();
  EXPECT_EQ(calls.load(), 3);
}

TEST(EventSender, FullQueueDropsOldest) {
  EventSender::Options o = fast();
  o.capacity = 2;
  std::vector<std::string> got;
  EventSender s(
      [&](const lm::TaskEvent& e, std::chrono::milliseconds) {
        got.push_back(e.task_id());
        return grpc::Status::OK;
      },
      o);
  s.enqueue(ev("a", lm::TaskEvent::DONE));
  s.enqueue(ev("b", lm::TaskEvent::DONE));
  s.enqueue(ev("c", lm::TaskEvent::DONE));
  EXPECT_EQ(s.dropped_full(), 1U);
  s.start();
  ASSERT_TRUE(wait_for([&] { return s.sent() == 2; }));
  s.stop();
  EXPECT_EQ(got, (std::vector<std::string>{"b", "c"}));
}

// ---------- TaskConfig ----------

TEST(TaskConfig, DefaultsAndValidation) {
  auto c = TaskConfig::parse(lm::Config::load_string("{}"), 4, nullptr);
  EXPECT_FALSE(c.enabled);
  EXPECT_EQ(c.listen, "0.0.0.0:50052");
  EXPECT_EQ(c.max_concurrent, 8);
  EXPECT_EQ(c.default_timeout_s, 300U);
  EXPECT_TRUE(c.allowed_commands.empty());
  c = TaskConfig::parse(lm::Config::load_string("task: {token: a}"), 4, "b");
  EXPECT_EQ(c.token, "b");
  try {
    TaskConfig::parse(lm::Config::load_string("task: {allowed_commands: [/bin/sh, sleep]}"), 4,
                      nullptr);
    ADD_FAILURE();
  } catch (const lm::ConfigError& e) {
    EXPECT_NE(std::string(e.what()).find("sleep"), std::string::npos);
  }
  EXPECT_THROW(TaskConfig::parse(lm::Config::load_string("task: {max_timeout_s: 10}"), 4, nullptr),
               lm::ConfigError);
}

TEST(TaskConfig, WorkerYamlParses) {
  const auto c = TaskConfig::parse(lm::Config::load_file(LM_CONFIG_DIR "/worker.yaml"), 2, nullptr);
  EXPECT_FALSE(c.enabled);
}

// ---------- TaskService（进程内 gRPC） ----------

struct FakeCollector final : public lm::Collector::Service {
  std::mutex mu;
  std::vector<std::string> events;
  grpc::Status Report(grpc::ServerContext*, const lm::Sample*, lm::Ack*) override {
    return grpc::Status::OK;
  }
  grpc::Status OnTaskEvent(grpc::ServerContext*, const lm::TaskEvent* e, lm::Ack*) override {
    std::lock_guard<std::mutex> lk(mu);
    events.push_back(e->task_id() + ":" + lm::TaskEvent::Kind_Name(e->kind()));
    return grpc::Status::OK;
  }
  std::vector<std::string> snapshot() {
    std::lock_guard<std::mutex> lk(mu);
    return events;
  }
};

struct Harness {
  FakeCollector collector;
  std::unique_ptr<grpc::Server> mgr;
  std::unique_ptr<EventSender> events;
  std::unique_ptr<TaskService> svc;
  std::unique_ptr<grpc::Server> worker;
  std::unique_ptr<lm::WorkerTask::Stub> stub;

  explicit Harness(TaskConfig cfg) {
    int mport = 0;
    grpc::ServerBuilder mb;
    mb.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &mport);
    mb.RegisterService(&collector);
    mgr = mb.BuildAndStart();
    events = std::make_unique<EventSender>(
        grpc::CreateChannel("127.0.0.1:" + std::to_string(mport), grpc::InsecureChannelCredentials()),
        EventSender::Options{});
    events->start();
    svc = std::make_unique<TaskService>(cfg, *events);
    int wport = 0;
    grpc::ServerBuilder wb;
    wb.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &wport);
    wb.RegisterService(svc.get());
    worker = wb.BuildAndStart();
    stub = lm::WorkerTask::NewStub(
        grpc::CreateChannel("127.0.0.1:" + std::to_string(wport), grpc::InsecureChannelCredentials()));
  }
  ~Harness() {
    worker->Shutdown();
    events->stop();
    mgr->Shutdown();
  }
  grpc::Status submit(const std::string& id, const std::string& cmd,
                      const std::vector<std::string>& args, lm::TaskResult* out,
                      uint32_t timeout_s = 0, const std::string& token = "") {
    grpc::ClientContext ctx;
    if (!token.empty()) ctx.AddMetadata("authorization", "Bearer " + token);
    lm::TaskReq req;
    req.set_task_id(id);
    req.set_command(cmd);
    for (const auto& a : args) req.add_args(a);
    req.set_timeout_s(timeout_s);
    return stub->Submit(&ctx, req, out);
  }
};

TaskConfig enabled_cfg() {
  TaskConfig c;
  c.enabled = true;
  c.allowed_commands = {"/bin/sh", "/bin/sleep"};
  c.max_concurrent = 4;
  return c;
}

TEST(TaskService, RunsAndReportsReceivedThenFailed) {
  Harness h(enabled_cfg());
  lm::TaskResult r;
  ASSERT_TRUE(h.submit("t1", "/bin/sh", {"-c", "echo hi; exit 3"}, &r).ok());
  EXPECT_EQ(r.task_id(), "t1");
  EXPECT_EQ(r.exit_code(), 3);
  EXPECT_EQ(r.stdout_tail(), "hi\n");
  ASSERT_TRUE(wait_for([&] { return h.collector.snapshot().size() == 2; }));
  EXPECT_EQ(h.collector.snapshot(), (std::vector<std::string>{"t1:RECEIVED", "t1:FAILED"}));
  ASSERT_TRUE(h.submit("t2", "/bin/sh", {"-c", "true"}, &r).ok());
  ASSERT_TRUE(wait_for([&] { return h.collector.snapshot().size() == 4; }));
  EXPECT_EQ(h.collector.snapshot()[3], "t2:DONE");
}

TEST(TaskService, Validation) {
  TaskConfig c = enabled_cfg();
  c.token = "s3cret";
  c.max_timeout_s = 600;
  Harness h(c);
  lm::TaskResult r;
  EXPECT_EQ(h.submit("t1", "/bin/sh", {"-c", "true"}, &r).error_code(),
            grpc::StatusCode::UNAUTHENTICATED);
  EXPECT_EQ(h.submit("t1", "/bin/sh", {"-c", "true"}, &r, 0, "wrong").error_code(),
            grpc::StatusCode::UNAUTHENTICATED);
  EXPECT_EQ(h.submit("t1", "/bin/rm", {"-rf", "/tmp/x"}, &r, 0, "s3cret").error_code(),
            grpc::StatusCode::PERMISSION_DENIED);
  EXPECT_EQ(h.submit("", "/bin/sh", {}, &r, 0, "s3cret").error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_EQ(h.submit(std::string(129, 'x'), "/bin/sh", {}, &r, 0, "s3cret").error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_EQ(h.submit("t1", "/bin/sh", {"-c", "true"}, &r, 601, "s3cret").error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_TRUE(h.submit("t1", "/bin/sh", {"-c", "true"}, &r, 0, "s3cret").ok());
  ASSERT_TRUE(wait_for([&] { return h.collector.snapshot().size() == 2; }));
  EXPECT_EQ(h.collector.snapshot()[0], "t1:RECEIVED");  // 被拒的请求不上报事件
  EXPECT_EQ(h.svc->rejected(), 6U);
}

TEST(TaskService, ConcurrencyLimitReportsFailed) {
  TaskConfig c = enabled_cfg();
  c.max_concurrent = 1;
  Harness h(c);
  std::thread slow([&] {
    lm::TaskResult r;
    EXPECT_TRUE(h.submit("slow", "/bin/sleep", {"1"}, &r).ok());
  });
  ASSERT_TRUE(wait_for([&] { return h.svc->running() == 1; }));
  lm::TaskResult r;
  EXPECT_EQ(h.submit("extra", "/bin/sleep", {"0"}, &r).error_code(),
            grpc::StatusCode::RESOURCE_EXHAUSTED);
  slow.join();
  ASSERT_TRUE(wait_for([&] { return h.collector.snapshot().size() == 3; }));
  const auto ev = h.collector.snapshot();
  EXPECT_NE(std::find(ev.begin(), ev.end(), "extra:FAILED"), ev.end());
  EXPECT_EQ(std::count(ev.begin(), ev.end(), "extra:RECEIVED"), 0);
}

TEST(TaskService, TimeoutReturnsMinusOne) {
  Harness h(enabled_cfg());
  lm::TaskResult r;
  ASSERT_TRUE(h.submit("t1", "/bin/sh", {"-c", "sleep 100 & sleep 100"}, &r, 1).ok());
  EXPECT_EQ(r.exit_code(), -1);
  EXPECT_GE(r.exec_ms(), 900U);
}

TEST(TaskService, Probe) {
  Harness h(enabled_cfg());
  grpc::ClientContext ctx;
  lm::ProbeReq req;
  req.set_seq(42);
  lm::ProbeResp resp;
  ASSERT_TRUE(h.stub->Probe(&ctx, req, &resp).ok());
  EXPECT_EQ(resp.seq(), 42U);
}

}  // namespace

// ---------- Rescue（M6） ----------

namespace {

class FakeCpu : public CpuSource {
 public:
  bool read(CpuSnapshot& out) override {
    tick_ += 1000;
    out.cores = {{0, tick_ / 2, tick_}};  // 忙碌度 0.5
    return true;
  }

 private:
  uint64_t tick_ = 0;
};

struct RescueHarness {
  FakeCollector collector;
  std::unique_ptr<grpc::Server> mgr;
  std::unique_ptr<EventSender> events;
  std::unique_ptr<RescueSampler> sampler;
  std::unique_ptr<TaskService> svc;
  std::unique_ptr<grpc::Server> worker;
  std::shared_ptr<grpc::Channel> ch;
  explicit RescueHarness(uint32_t max_s = 15) {
    events = std::make_unique<EventSender>(
        [](const lm::TaskEvent&, std::chrono::milliseconds) { return grpc::Status::OK; },
        EventSender::Options{});
    sampler = std::make_unique<RescueSampler>(
        [] { return std::make_unique<FakeCpu>(); },
        [] {
          lm::Sample s;
          s.set_node_id("w1");
          s.set_mem_avail_ratio(0.7F);
          return s;
        });
    TaskConfig c = enabled_cfg();
    c.rescue_max_s = max_s;
    svc = std::make_unique<TaskService>(c, *events, sampler.get());
    int port = 0;
    grpc::ServerBuilder b;
    b.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    b.RegisterService(svc.get());
    worker = b.BuildAndStart();
    ch = grpc::CreateChannel("127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials());
  }
};

TEST(WorkerRescue, EchoesSeqWithFreshSample) {
  RescueHarness h;
  auto stub = lm::WorkerTask::NewStub(h.ch);
  grpc::ClientContext ctx;
  auto stream = stub->Rescue(&ctx);
  int got = 0;
  for (uint64_t i = 1; i <= 1000; ++i) {
    lm::RescueReq req;
    req.set_seq(i);
    req.set_send_ts_us(static_cast<int64_t>(i) * 7);
    ASSERT_TRUE(stream->Write(req));
    lm::RescueResp resp;
    ASSERT_TRUE(stream->Read(&resp));
    EXPECT_EQ(resp.seq(), i);
    EXPECT_EQ(resp.send_ts_us(), static_cast<int64_t>(i) * 7);
    EXPECT_EQ(resp.sample().node_id(), "w1");
    EXPECT_FLOAT_EQ(resp.sample().mem_avail_ratio(), 0.7F);
    EXPECT_NEAR(resp.sample().cpu_busy(), 0.5, 1e-6);
    EXPECT_GT(resp.sample().ts_ms(), 0);
    ++got;
  }
  stream->WritesDone();
  EXPECT_TRUE(stream->Finish().ok());
  EXPECT_EQ(got, 1000);
  EXPECT_FALSE(h.sampler->active());
}

TEST(WorkerRescue, SecondStreamAborted) {
  RescueHarness h;
  auto stub = lm::WorkerTask::NewStub(h.ch);
  grpc::ClientContext c1;
  auto s1 = stub->Rescue(&c1);
  lm::RescueReq req;
  req.set_seq(1);
  ASSERT_TRUE(s1->Write(req));
  lm::RescueResp resp;
  ASSERT_TRUE(s1->Read(&resp));
  ASSERT_TRUE(wait_for([&] { return h.svc->rescue_active(); }));
  grpc::ClientContext c2;
  auto s2 = stub->Rescue(&c2);
  s2->WritesDone();
  EXPECT_EQ(s2->Finish().error_code(), grpc::StatusCode::ABORTED);
  req.set_seq(2);
  ASSERT_TRUE(s1->Write(req));
  ASSERT_TRUE(s1->Read(&resp));
  EXPECT_EQ(resp.seq(), 2U);
  s1->WritesDone();
  EXPECT_TRUE(s1->Finish().ok());
}

TEST(WorkerRescue, MaxDurationEnforced) {
  RescueHarness h(1);
  auto stub = lm::WorkerTask::NewStub(h.ch);
  grpc::ClientContext ctx;
  auto stream = stub->Rescue(&ctx);
  const auto t0 = std::chrono::steady_clock::now();
  lm::RescueResp resp;
  for (uint64_t i = 1;; ++i) {
    lm::RescueReq req;
    req.set_seq(i);
    if (!stream->Write(req) || !stream->Read(&resp)) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    ASSERT_LT(std::chrono::steady_clock::now() - t0, std::chrono::seconds(3));
  }
  EXPECT_EQ(stream->Finish().error_code(), grpc::StatusCode::DEADLINE_EXCEEDED);
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0).count();
  EXPECT_GE(ms, 950);
  EXPECT_FALSE(h.sampler->active());
}

}  // namespace
