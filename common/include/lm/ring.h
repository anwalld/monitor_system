#pragma once

// 采集与上报解耦用的 CAS 环形缓冲：单生产者（采集线程）、单消费者（上报线程）。
// 生产者轮流写槽，覆盖最旧的 READY，跳过消费者正占着的 READING 槽；
// 消费者取 seq 最大的 READY，拷出后立即释放槽。槽状态的 release/acquire 保证数据可见性。

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace lm {

template <typename T, std::size_t N = 8>
class Ring {
  static_assert(N >= 2, "Ring needs at least 2 slots");

 public:
  enum State : uint8_t { kEmpty = 0, kWriting = 1, kReady = 2, kReading = 3 };

  // 写入一条样本，返回分配给它的 seq（从 1 开始）；所有槽都被占用时返回 0（实际不会发生：
  // 单消费者最多占一个槽）。只能由一个生产者线程调用。
  uint64_t publish(const T& value) {
    for (std::size_t k = 0; k < N; ++k) {
      Slot& slot = slots_[(cursor_ + k) % N];
      uint8_t st = slot.state.load(std::memory_order_relaxed);
      if (st == kReading || st == kWriting) continue;
      if (!slot.state.compare_exchange_strong(st, kWriting, std::memory_order_acquire,
                                              std::memory_order_relaxed)) {
        continue;
      }
      slot.data = value;
      const uint64_t seq = ++seq_;
      slot.seq.store(seq, std::memory_order_relaxed);
      slot.state.store(kReady, std::memory_order_release);  // 与消费者的 acquire 配对
      cursor_ = (cursor_ + k + 1) % N;
      return seq;
    }
    return 0;
  }

  // 取出 seq 最大、且比上次取到的更新的 READY 样本；没有可取的返回 false。
  // 因此连续调用拿到的 seq 严格递增，旧样本留在槽里等生产者覆盖。只能由一个消费者线程调用。
  bool take_latest(T& out, uint64_t* seq_out = nullptr) {
    for (;;) {
      int best = -1;
      uint64_t best_seq = last_taken_;
      for (std::size_t i = 0; i < N; ++i) {
        if (slots_[i].state.load(std::memory_order_acquire) != kReady) continue;
        const uint64_t q = slots_[i].seq.load(std::memory_order_relaxed);
        if (q > best_seq) {
          best = static_cast<int>(i);
          best_seq = q;
        }
      }
      if (best < 0) return false;
      uint8_t st = kReady;
      Slot& slot = slots_[best];
      if (!slot.state.compare_exchange_strong(st, kReading, std::memory_order_acquire,
                                              std::memory_order_relaxed)) {
        continue;  // 刚被生产者拿去覆盖，重新找
      }
      // 抢到槽后重新读 seq：扫描与 CAS 之间槽可能已被覆盖成更新的样本
      const uint64_t seq = slot.seq.load(std::memory_order_relaxed);
      out = slot.data;
      slot.state.store(kEmpty, std::memory_order_release);
      last_taken_ = seq;
      if (seq_out != nullptr) *seq_out = seq;
      return true;
    }
  }

  static constexpr std::size_t capacity() { return N; }

 private:
  struct alignas(64) Slot {
    std::atomic<uint8_t> state{kEmpty};
    std::atomic<uint64_t> seq{0};
    T data{};
  };

  Slot slots_[N];
  std::size_t cursor_ = 0;  // 仅生产者使用
  uint64_t seq_ = 0;        // 仅生产者使用
  uint64_t last_taken_ = 0; // 仅消费者使用
};

}  // namespace lm
