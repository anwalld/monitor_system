#pragma once

// seqlock 读端：按“seq 奇数表示正在写”的协议一致性读取共享内存。
//
// 写端（cpustat 内核模块）的顺序是：seq+1（变奇）→ 写屏障 → 写数据 → 写屏障 → seq+1（变偶）。
// 读端：读 seq（acquire），奇数则重试；拷贝数据；acquire fence；再读 seq，前后相同才接受。
//
// 拷贝期间写端可能并发修改，按 C++ 内存模型这是数据竞争；这是 seqlock 的通用做法，
// 撕裂的结果由前后两次 seq 比较丢弃。TSan 构建下用 race:lm::seqlock_read 抑制。

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>

#include "cpustat_uapi.h"

namespace lm {

inline constexpr int kSeqlockMaxRetry = 100;

// 通用读取：copy() 负责把数据拷到调用方缓冲。成功返回 true；重试 kSeqlockMaxRetry 次仍不一致返回 false。
template <typename Copy>
#if defined(__clang__) || defined(__GNUC__)
__attribute__((noinline))
#endif
bool seqlock_read(const uint32_t* seq, Copy&& copy) {
  for (int retry = 0; retry < kSeqlockMaxRetry; ++retry) {
    const uint32_t s1 = __atomic_load_n(seq, __ATOMIC_ACQUIRE);
    if (s1 & 1U) continue;  // 正在写
    copy();
    std::atomic_thread_fence(std::memory_order_acquire);
    const uint32_t s2 = __atomic_load_n(seq, __ATOMIC_RELAXED);
    if (s1 == s2) return true;  // 前后一致，快照可用
  }
  return false;
}

// 读取 cpustat 共享页：把最多 CPUSTAT_MAX_CPU 条每核记录拷到 buf，ncpu 写入实际条数。
inline bool read_cpustat(const cpustat_shm* shm, cpustat_core* buf, uint32_t& ncpu) {
  uint32_t n = 0;
  const bool ok = seqlock_read(&shm->seq, [&] {
    n = std::min<uint32_t>(__atomic_load_n(&shm->ncpu, __ATOMIC_RELAXED), CPUSTAT_MAX_CPU);
    std::memcpy(buf, shm->cpu, sizeof(cpustat_core) * n);
  });
  if (ok) ncpu = n;
  return ok;
}

}  // namespace lm
