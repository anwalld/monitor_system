// Advisor gRPC 服务（进程内，M9）：推送后节点表里有因子、ttl 为 0 用默认、校验与 enabled=false。

#include <gtest/gtest.h>

#include <grpcpp/grpcpp.h>

#include "api/advisor_service.h"
#include "lm/clock.h"

using namespace lm::manager;

namespace {

struct Harness {
  NodeTable table;
  AdvisorService svc{table};
  std::unique_ptr<grpc::Server> server;
  std::unique_ptr<lm::Advisor::Stub> stub;
  explicit Harness(NodeTable::Options o = {}) : table(o) {
    int port = 0;
    grpc::ServerBuilder b;
    b.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    b.RegisterService(&svc);
    server = b.BuildAndStart();
    stub = lm::Advisor::NewStub(grpc::CreateChannel("127.0.0.1:" + std::to_string(port),
                                                    grpc::InsecureChannelCredentials()));
    lm::Sample s;
    s.set_node_id("w1");
    s.set_seq(1);
    s.add_core_busy(0.1F);
    s.set_cpu_busy(0.1F);
    s.set_mem_avail_ratio(0.8F);
    table.apply(s, "10.0.0.1", lm::mono_ms(), lm::wall_ms(), nullptr);
  }
  grpc::Status push(const std::string& node, float cpu, float factor, uint32_t ttl) {
    grpc::ClientContext ctx;
    lm::PredictionBatch req;
    req.set_ts_ms(lm::wall_ms());
    req.set_ttl_ms(ttl);
    auto* p = req.add_items();
    p->set_node_id(node);
    p->set_cpu_busy_30s(cpu);
    p->set_factor(factor);
    lm::Ack ack;
    return stub->PushPredictions(&ctx, req, &ack);
  }
};

TEST(AdvisorService, PushAppliesFactorWithDefaultTtl) {
  Harness h;
  ASSERT_TRUE(h.push("w1", 0.85F, 0.75F, 0).ok());
  EXPECT_NEAR(*h.table.f_pred("w1", lm::mono_ms()), 0.75, 1e-6);
  EXPECT_DOUBLE_EQ(*h.table.f_pred("w1", lm::mono_ms() + 10001), 1.0);  // 默认 10 秒后过期
  EXPECT_EQ(h.svc.batches(), 1U);
  EXPECT_EQ(h.svc.applied(), 1U);
  ASSERT_TRUE(h.push("nope", 0.5F, 0.5F, 5000).ok());  // 未知节点不报错
  EXPECT_EQ(h.svc.unknown_nodes(), 1U);
}

TEST(AdvisorService, RejectsEmptyNodeId) {
  Harness h;
  EXPECT_EQ(h.push("", 0.5F, 0.5F, 1000).error_code(), grpc::StatusCode::INVALID_ARGUMENT);
}

TEST(AdvisorService, DisabledKeepsFactorOne) {
  NodeTable::Options o;
  o.advisor.enabled = false;
  Harness h(o);
  ASSERT_TRUE(h.push("w1", 0.9F, 0.5F, 10000).ok());
  EXPECT_DOUBLE_EQ(*h.table.f_pred("w1", lm::mono_ms()), 1.0);
  EXPECT_EQ(h.svc.applied(), 0U);
}

}  // namespace
