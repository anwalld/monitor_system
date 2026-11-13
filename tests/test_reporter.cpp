#include <gtest/gtest.h>

#include <atomic>
#include <mutex>
#include <thread>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "monitor.grpc.pb.h"
#include "reporter/reporter.h"

using lm::worker::Reporter;

namespace {

class FakeCollector final : public lm::Collector::Service {
 public:
  grpc::Status Report(grpc::ServerContext*, const lm::Sample* s, lm::Ack*) override {
    if (delay_ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    std::lock_guard<std::mutex> lk(mu);
    seqs.push_back(s->seq());
    return grpc::Status::OK;
  }
  std::atomic<int> delay_ms{0};
  std::mutex mu;
  std::vector<uint64_t> seqs;
};

struct Server {
  FakeCollector svc;
  std::unique_ptr<grpc::Server> server;
  int port = 0;
  Server() {
    grpc::ServerBuilder b;
    b.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    b.RegisterService(&svc);
    server = b.BuildAndStart();
  }
  std::string addr() const { return "127.0.0.1:" + std::to_string(port); }
};

lm::Sample sample(uint64_t seq) {
  lm::Sample s;
  s.set_node_id("w1");
  s.set_seq(seq);
  return s;
}

Reporter::Options opts() {
  Reporter::Options o;
  o.interval = std::chrono::milliseconds(50);
  o.deadline = std::chrono::milliseconds(200);
  return o;
}

TEST(Reporter, SendsLatestOnlyOnceAndSkipsWhenNothingNew) {
  Server srv;
  Reporter::SampleRing ring;
  Reporter rep(ring, grpc::CreateChannel(srv.addr(), grpc::InsecureChannelCredentials()), opts());
  ring.publish(sample(1));
  ring.publish(sample(2));
  ring.publish(sample(3));
  EXPECT_TRUE(rep.report_once());   // 发 3
  EXPECT_FALSE(rep.report_once());  // 取到 2，比已发的 3 旧，跳过
  EXPECT_FALSE(rep.report_once());  // 取到 1，跳过
  EXPECT_FALSE(rep.report_once());  // 缓冲已空
  EXPECT_EQ(rep.last_sent_seq(), 3U);
  std::lock_guard<std::mutex> lk(srv.svc.mu);
  EXPECT_EQ(srv.svc.seqs, (std::vector<uint64_t>{3}));
  EXPECT_EQ(rep.sent_ok(), 1U);
}

TEST(Reporter, DeadlineExceededCountsAsFailureWithoutRetry) {
  Server srv;
  srv.svc.delay_ms = 500;
  Reporter::SampleRing ring;
  Reporter rep(ring, grpc::CreateChannel(srv.addr(), grpc::InsecureChannelCredentials()), opts());
  ring.publish(sample(1));
  const auto t0 = std::chrono::steady_clock::now();
  EXPECT_TRUE(rep.report_once());
  const auto elapsed = std::chrono::steady_clock::now() - t0;
  EXPECT_LT(elapsed, std::chrono::milliseconds(450));  // 约 200ms 放弃
  EXPECT_EQ(rep.sent_failed(), 1U);
  EXPECT_FALSE(rep.report_once());  // 不重发同一条
}

TEST(Reporter, UnreachableManagerFailsFast) {
  Reporter::SampleRing ring;
  Reporter rep(ring, grpc::CreateChannel("127.0.0.1:1", grpc::InsecureChannelCredentials()), opts());
  ring.publish(sample(1));
  EXPECT_TRUE(rep.report_once());
  EXPECT_EQ(rep.sent_failed(), 1U);
  EXPECT_EQ(rep.sent_ok(), 0U);
}

TEST(Reporter, ThreadSendsStrictlyIncreasingSeqs) {
  Server srv;
  Reporter::SampleRing ring;
  Reporter rep(ring, grpc::CreateChannel(srv.addr(), grpc::InsecureChannelCredentials()), opts());
  rep.start();
  for (uint64_t i = 1; i <= 30; ++i) {
    ring.publish(sample(i));
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(120));
  rep.stop();
  std::lock_guard<std::mutex> lk(srv.svc.mu);
  ASSERT_GE(srv.svc.seqs.size(), 3U);
  for (std::size_t i = 1; i < srv.svc.seqs.size(); ++i) EXPECT_GT(srv.svc.seqs[i], srv.svc.seqs[i - 1]);
  EXPECT_EQ(srv.svc.seqs.back(), 30U);
}

}  // namespace
