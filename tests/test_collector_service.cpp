#include <gtest/gtest.h>

#include <grpcpp/grpcpp.h>

#include "ingest/collector_service.h"

using namespace lm::manager;

namespace {

struct Harness {
  NodeTable table{NodeTable::Options{}};
  BoundedQueue<AcceptedSample> queue{16};
  CollectorService svc{table, queue};
  std::unique_ptr<grpc::Server> server;
  std::unique_ptr<lm::Collector::Stub> stub;
  Harness() {
    int port = 0;
    grpc::ServerBuilder b;
    b.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    b.RegisterService(&svc);
    server = b.BuildAndStart();
    stub = lm::Collector::NewStub(
        grpc::CreateChannel("127.0.0.1:" + std::to_string(port), grpc::InsecureChannelCredentials()));
  }
  grpc::Status report(const lm::Sample& s) {
    grpc::ClientContext ctx;
    lm::Ack ack;
    return stub->Report(&ctx, s, &ack);
  }
  grpc::Status event(lm::TaskEvent::Kind k) {
    grpc::ClientContext ctx;
    lm::TaskEvent e;
    e.set_task_id("t1");
    e.set_kind(k);
    lm::Ack ack;
    return stub->OnTaskEvent(&ctx, e, &ack);
  }
};

TEST(CollectorService, ReportAcceptedAndQueued) {
  Harness h;
  lm::Sample s;
  s.set_node_id("w1");
  s.set_seq(1);
  s.add_core_busy(0.5F);
  ASSERT_TRUE(h.report(s).ok());
  auto n = h.table.get("w1");
  ASSERT_TRUE(n);
  EXPECT_EQ(n->addr, "127.0.0.1:50052");
  auto batch = h.queue.pop_batch(10, std::chrono::milliseconds(0));
  ASSERT_EQ(batch.size(), 1U);
  EXPECT_EQ(batch[0].sample.seq(), 1U);
}

TEST(CollectorService, StaleSampleStillOkButNotQueued) {
  Harness h;
  lm::Sample s;
  s.set_node_id("w1");
  s.set_seq(5);
  ASSERT_TRUE(h.report(s).ok());
  s.set_seq(4);
  ASSERT_TRUE(h.report(s).ok());
  EXPECT_EQ(h.queue.size(), 1U);
  EXPECT_EQ(h.svc.stale(), 1U);
}

TEST(CollectorService, EmptyNodeIdIsInvalidArgument) {
  Harness h;
  lm::Sample s;
  s.set_seq(1);
  EXPECT_EQ(h.report(s).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
  EXPECT_EQ(h.table.counts(0).total, 0U);
}

TEST(CollectorService, TaskEventKindValidation) {
  Harness h;
  EXPECT_TRUE(h.event(lm::TaskEvent::RECEIVED).ok());
  EXPECT_EQ(h.event(lm::TaskEvent::KIND_UNSPECIFIED).error_code(),
            grpc::StatusCode::INVALID_ARGUMENT);
}

}  // namespace
