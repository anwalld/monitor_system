#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <memory>
#include <thread>
#include <vector>

#include "cpustat_uapi.h"
#include "lm/seqlock_reader.h"

namespace {

TEST(CpustatUapi, Layout) {
  EXPECT_EQ(sizeof(cpustat_shm), 6160U);
  EXPECT_EQ(offsetof(cpustat_shm, cpu), 16U);
  EXPECT_EQ(offsetof(cpustat_core, stamp_us), 16U);
}

TEST(Seqlock, ReadsConsistentSnapshot) {
  auto shm = std::make_unique<cpustat_shm>();
  shm->seq = 4;
  shm->ncpu = 2;
  shm->cpu[0] = {10, 1, 100};
  shm->cpu[1] = {20, 2, 101};
  cpustat_core buf[CPUSTAT_MAX_CPU];
  uint32_t n = 0;
  ASSERT_TRUE(lm::read_cpustat(shm.get(), buf, n));
  EXPECT_EQ(n, 2U);
  EXPECT_EQ(buf[1].idle_us, 20U);
  EXPECT_EQ(buf[1].stamp_us, 101U);
}

TEST(Seqlock, FailsWhenWriterStuckOdd) {
  auto shm = std::make_unique<cpustat_shm>();
  shm->seq = 7;
  shm->ncpu = 1;
  cpustat_core buf[CPUSTAT_MAX_CPU];
  uint32_t n = 99;
  EXPECT_FALSE(lm::read_cpustat(shm.get(), buf, n));
  EXPECT_EQ(n, 99U);  // 失败时不改输出
}

TEST(Seqlock, TruncatesNcpu) {
  auto shm = std::make_unique<cpustat_shm>();
  shm->seq = 2;
  shm->ncpu = 1000;
  cpustat_core buf[CPUSTAT_MAX_CPU];
  uint32_t n = 0;
  ASSERT_TRUE(lm::read_cpustat(shm.get(), buf, n));
  EXPECT_EQ(n, static_cast<uint32_t>(CPUSTAT_MAX_CPU));
}

// 写线程按奇偶协议把前后两半写成同一个递增值；读线程校验每次成功读到的两半一致。
TEST(Seqlock, ConcurrentWriterNeverTears) {
  auto shm = std::make_unique<cpustat_shm>();
  shm->ncpu = CPUSTAT_MAX_CPU;
  constexpr int kReaders = 4;
  constexpr int kReadsEach = 100000;
  constexpr int kHalf = CPUSTAT_MAX_CPU / 2;

  std::atomic<int> readers_done{0};
  std::thread writer([&] {
    uint64_t v = 0;
    while (readers_done.load(std::memory_order_relaxed) < kReaders) {
      ++v;
      const uint32_t s = __atomic_load_n(&shm->seq, __ATOMIC_RELAXED);
      __atomic_store_n(&shm->seq, s + 1, __ATOMIC_RELAXED);  // 变奇数
      std::atomic_thread_fence(std::memory_order_seq_cst);
      for (int i = 0; i < CPUSTAT_MAX_CPU; ++i) {
        shm->cpu[i].idle_us = v;
        shm->cpu[i].stamp_us = v;
      }
      __atomic_store_n(&shm->seq, s + 2, __ATOMIC_RELEASE);  // 变偶数
      // 真实写者是周期刷新；写完留一小段空档，让读者既有机会读成功，也经常与写入重叠
      for (int spin = 0; spin < 2000; ++spin) {
        (void)__atomic_load_n(&shm->ncpu, __ATOMIC_RELAXED);
      }
    }
  });

  std::vector<std::thread> readers;
  std::atomic<int> torn{0};
  std::atomic<int> ok_reads{0};
  for (int r = 0; r < kReaders; ++r) {
    readers.emplace_back([&] {
      auto buf = std::make_unique<cpustat_core[]>(CPUSTAT_MAX_CPU);
      for (int i = 0; i < kReadsEach; ++i) {
        uint32_t n = 0;
        if (!lm::read_cpustat(shm.get(), buf.get(), n)) continue;
        ok_reads.fetch_add(1, std::memory_order_relaxed);
        const uint64_t first = buf[0].idle_us;
        for (int k = 0; k < CPUSTAT_MAX_CPU; ++k) {
          // 前半与后半、idle 与 stamp 都必须是同一次写入的值
          if (buf[k].idle_us != first || buf[k].stamp_us != first ||
              buf[k < kHalf ? k + kHalf : k - kHalf].idle_us != first) {
            torn.fetch_add(1, std::memory_order_relaxed);
            break;
          }
        }
      }
      readers_done.fetch_add(1, std::memory_order_relaxed);
    });
  }
  for (auto& t : readers) t.join();
  writer.join();
  EXPECT_EQ(torn.load(), 0);
  EXPECT_GT(ok_reads.load(), 1000);  // 足够多的成功读取，校验才有意义
}

}  // namespace
