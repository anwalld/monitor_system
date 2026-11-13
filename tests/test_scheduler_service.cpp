// Scheduler gRPC 服务（进程内）：Allocate 的返回、错误码与 task_id。

#include <gtest/gtest.h>

#include <grpcpp/grpcpp.h>

#include "api/scheduler_service.h"
#include "lm/clock.h"

using namespace lm::manager;

namespace {

lm::Sample healthy(const std::string& id, uint64_t seq, float cpu = 0.1F) {
  lm::Sample s;
  s.set_node_id(id);
  s.set_seq(seq);
  s.add_core_busy(cpu);
  s.set_cpu_busy(cpu);
  s.set_mem_avail_ratio(0.8F);
  s.set_runq_p99_us(50.0F);
  return s;
}

struct Harness {
  NodeTable table{NodeTable::Options{}};
  SchedulerService svc{table, SchedulerService::Options{0x19a7c3e2f10, 7, 64}};
  std::unique_ptr<grpc::Server> server;
  std::unique_ptr<lm::Scheduler::Stub> stub;
  Harness() {
    int port = 0;
    grpc::ServerBuilder b;
    b.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    b.RegisterService(&svc);
    server = b.BuildAndStart();
    stub = lm::Scheduler::NewStub(grpc::CreateChannel("127.0.0.1:" + std::to_string(port),
                                                      grpc::InsecureChannelCredentials()));
  }
  grpc::Status allocate(const std::string& client, lm::AllocateResp* resp) {
    grpc::ClientContext ctx;
    lm::AllocateReq req;
    req.set_client_id(client);
    req.set_input_bytes(1024);
    req.set_kind("cpu");
    return stub->Allocate(&ctx, req, resp);
  }
  void report(const lm::Sample& s, const std::string& ip) {
    table.apply(s, ip, lm::mono_ms(), lm::wall_ms(), nullptr);
  }
};

TEST(SchedulerService, AllocatesAndIncrementsTaskId) {
  Harness h;
  h.report(healthy("w1", 1), "10.0.0.1");
  lm::AllocateResp r;
  ASSERT_TRUE(h.allocate("bench", &r).ok());
  EXPECT_EQ(r.worker_addr(), "10.0.0.1:50052");
  EXPECT_EQ(r.task_id(), "19a7c3e2f10-1");
  ASSERT_TRUE(h.allocate("bench", &r).ok());
  EXPECT_EQ(r.task_id(), "19a7c3e2f10-2");
  EXPECT_EQ(h.svc.allocated(), 2U);
}

TEST(SchedulerService, NoNodesIsResourceExhausted) {
  Harness h;
  lm::AllocateResp r;
  const auto st = h.allocate("bench", &r);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::RESOURCE_EXHAUSTED);
  EXPECT_NE(st.error_message().find("总数 0"), std::string::npos) << st.error_message();
  EXPECT_EQ(h.svc.rejected(), 1U);
}

TEST(SchedulerService, AllEvictedIsResourceExhausted) {
  Harness h;
  for (uint64_t i = 1; i <= 3; ++i) h.report(healthy("w1", i, 0.98F), "10.0.0.1");
  lm::AllocateResp r;
  const auto st = h.allocate("bench", &r);
  EXPECT_EQ(st.error_code(), grpc::StatusCode::RESOURCE_EXHAUSTED);
  EXPECT_NE(st.error_message().find("总数 1"), std::string::npos) << st.error_message();
  EXPECT_NE(st.error_message().find("剔除 1"), std::string::npos) << st.error_message();
}

TEST(SchedulerService, InvalidClientId) {
  Harness h;
  h.report(healthy("w1", 1), "10.0.0.1");
  lm::AllocateResp r;
  EXPECT_EQ(h.allocate("", &r).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_EQ(h.allocate(std::string(65, 'x'), &r).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_TRUE(h.allocate(std::string(64, 'x'), &r).ok());
}

TEST(SchedulerService, MarkSent) {
  Harness h;
  h.report(healthy("w1", 1), "10.0.0.1");
  lm::AllocateResp r;
  ASSERT_TRUE(h.allocate("bench", &r).ok());
  auto mark = [&](const std::string& id) {
    grpc::ClientContext ctx;
    lm::TaskRef ref;
    ref.set_task_id(id);
    lm::Ack ack;
    return h.stub->MarkSent(&ctx, ref, &ack).error_code();
  };
  EXPECT_EQ(mark(r.task_id()), grpc::StatusCode::OK);
  EXPECT_EQ(mark(r.task_id()), grpc::StatusCode::OK);
  EXPECT_EQ(mark("nope-1"), grpc::StatusCode::NOT_FOUND);
  EXPECT_EQ(mark(""), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_EQ(h.table.inflight_snapshot("w1").inflight_tasks, 1);
}

TEST(SchedulerService, AllocateRecordsInputBytes) {
  Harness h;
  h.report(healthy("w1", 1), "10.0.0.1");
  lm::AllocateResp r;
  ASSERT_TRUE(h.allocate("bench", &r).ok());  // input_bytes = 1024
  EXPECT_EQ(h.table.inflight_snapshot("w1").inflight_bytes, 1024);
}

TEST(SchedulerService, SpreadsAcrossNodesByWeight) {
  Harness h;
  h.report(healthy("w1", 1), "10.0.0.1");
  h.report(healthy("w2", 1, 0.55F), "10.0.0.2");  // 0.85
  int w1 = 0;
  const int n = 2000;
  for (int i = 0; i < n; ++i) {
    lm::AllocateResp r;
    ASSERT_TRUE(h.allocate("bench", &r).ok());
    if (r.worker_addr() == "10.0.0.1:50052") ++w1;
  }
  EXPECT_NEAR(static_cast<double>(w1) / n, 1.0 / 1.85, 0.04);
}

}  // namespace
