// CPU 读取耗时基准：对比 cpustat 共享页与 /proc/stat 两种来源读取一次快照（含转成 CpuSnapshot）的耗时。
// 用法：cpu_read_bench [--iters 100000] [--warmup 1000] [--cpu 1] [--batch 100]
// 方法：绑核；先测空计时取中位数作为计时开销；每种来源预热后测量，扣除开销后报 P50、P99。
// 分批计时：每 batch 次读取计一次时，除以 batch 得到单次耗时。虚拟机上 arm64 计时器一跳约 41ns，
// 比一次共享页读取还长，逐次计时分辨不出来；--batch 1 退化为逐次计时。
// 验收（开发文档 M2）：共享页 P50 不超过 /proc/stat P50 的 15%。

#if defined(__linux__)
#include <sched.h>
#endif
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "collector/proc_readers.h"
#include "collector/shm_cpu_source.h"

namespace {

inline int64_t now_ns() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

template <typename T>
double percentile(std::vector<T>& v, double p) {
  const auto k = static_cast<std::size_t>(p * static_cast<double>(v.size() - 1));
  std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(k), v.end());
  return static_cast<double>(v[k]);
}

struct Result {
  double p50 = 0;
  double p99 = 0;
  long failures = 0;
};

Result measure(lm::worker::CpuSource& src, int warmup, int iters, int batch, int64_t overhead) {
  lm::worker::CpuSnapshot snap;
  Result r;
  for (int i = 0; i < warmup; ++i) (void)src.read(snap);
  const int batches = std::max(1, iters / batch);
  std::vector<double> t(static_cast<std::size_t>(batches));
  for (int i = 0; i < batches; ++i) {
    const int64_t a = now_ns();
    for (int k = 0; k < batch; ++k) {
      if (!src.read(snap)) ++r.failures;
    }
    const int64_t b = now_ns();
    t[static_cast<std::size_t>(i)] =
        static_cast<double>(std::max<int64_t>(0, b - a - overhead)) / batch;
  }
  r.p50 = percentile(t, 0.50);
  r.p99 = percentile(t, 0.99);
  return r;
}

}  // namespace

int main(int argc, char** argv) {
  int iters = 100000;
  int warmup = 1000;
  int cpu = 1;
  int batch = 100;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--iters" && i + 1 < argc) {
      iters = std::atoi(argv[++i]);
    } else if (a == "--warmup" && i + 1 < argc) {
      warmup = std::atoi(argv[++i]);
    } else if (a == "--cpu" && i + 1 < argc) {
      cpu = std::atoi(argv[++i]);
    } else if (a == "--batch" && i + 1 < argc) {
      batch = std::max(1, std::atoi(argv[++i]));
    } else {
      std::fprintf(stderr, "用法: cpu_read_bench [--iters N] [--warmup N] [--cpu N] [--batch N]\n");
      return 2;
    }
  }
  if (iters < 100) iters = 100;

#if defined(__linux__)
  cpu_set_t set;
  CPU_ZERO(&set);
  CPU_SET(cpu, &set);
  if (sched_setaffinity(0, sizeof(set), &set) != 0) {
    std::fprintf(stderr, "绑定到 CPU %d 失败：%s\n", cpu, std::strerror(errno));
    return 1;
  }
  std::printf("pinned to cpu %d, warmup %d, iters %d, batch %d\n", cpu, warmup, iters, batch);
#else
  std::printf("(非 Linux：不绑核) warmup %d, iters %d, batch %d\n", warmup, iters, batch);
#endif

  // 计时开销：连续两次取时间的差，取中位数
  std::vector<int64_t> ov(10000);
  for (auto& x : ov) {
    const int64_t a = now_ns();
    x = now_ns() - a;
  }
  const auto overhead = static_cast<int64_t>(percentile(ov, 0.5));
  std::printf("timer overhead %lld ns (subtracted)\n", static_cast<long long>(overhead));

  lm::worker::ProcCpuSource proc;
  const Result rp = measure(proc, warmup, iters, batch, overhead);
  std::printf("/proc/stat  P50 %9.1f ns  P99 %9.1f ns  failures %ld\n", rp.p50, rp.p99,
              rp.failures);

  std::unique_ptr<lm::worker::ShmCpuSource> shm;
  try {
    shm = std::make_unique<lm::worker::ShmCpuSource>();
  } catch (const std::exception& e) {
    std::printf("cpustat     unavailable (%s), only /proc/stat measured\n", e.what());
    return 0;
  }
  const Result rs = measure(*shm, warmup, iters, batch, overhead);
  std::printf("cpustat shm P50 %9.1f ns  P99 %9.1f ns  failures %ld\n", rs.p50, rs.p99,
              rs.failures);
  const double ratio = rp.p50 > 0 ? rs.p50 / rp.p50 : 0;
  std::printf("shm/proc P50 ratio %.3f (reduction %.1f%%) -> %s\n", ratio, (1 - ratio) * 100,
              ratio <= 0.15 ? "PASS (<= 0.15)" : "FAIL (> 0.15)");
  return ratio <= 0.15 ? 0 : 1;
}
