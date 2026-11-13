#pragma once

// 每块网卡的探测状态（纯逻辑，时钟由调用方传入，便于测试）：
// 待确认队列（按发送顺序）、回包匹配、队头超时、迟到回包丢弃、周期累计与汇总。

#include <cstdint>
#include <deque>

namespace lm::worker {

struct ProbePeriod {
  double rtt_ms = 0.0;
  double jitter_ms = 0.0;
  double timeout_rate = 0.0;
  uint64_t ok = 0;
  uint64_t timeouts = 0;
};

class ProbeState {
 public:
  explicit ProbeState(int64_t timeout_ns) : timeout_ns_(timeout_ns) {}

  // 记录一次已发出的请求。
  void on_sent(uint16_t seq, uint32_t target, int64_t sent_ns);
  // 发送失败：直接计一次超时。
  void on_send_failed() { ++timeouts_; }
  // 收到回包：seq 在队列中且来源是该请求的目标时计一次成功并返回 true；否则（迟到、伪造）丢弃返回 false。
  bool on_reply(uint16_t seq, uint32_t src, int64_t recv_ns);
  // 队头等待超过超时时间的请求计超时并出队，返回本次新增的超时数。
  int expire(int64_t now_ns);

  // 取出本周期结果并清零（待确认队列保留）。
  ProbePeriod take();

  std::size_t pending() const { return pending_.size(); }

 private:
  struct Pending {
    uint16_t seq;
    uint32_t target;
    int64_t sent_ns;
  };
  int64_t timeout_ns_;
  std::deque<Pending> pending_;
  uint64_t ok_ = 0;
  uint64_t timeouts_ = 0;
  double sum_ms_ = 0.0;
  double sumsq_ms_ = 0.0;
};

}  // namespace lm::worker
