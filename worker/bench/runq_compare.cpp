// 调度延迟对照工具：加载与 Worker 相同的 eBPF 程序，统计一段时间内的分布，输出 JSON，
// 供 bpf/test/compare_runqlat.py 与 bcc runqlat 对比。同时测一次读取并合并 map 的耗时。
// 用法：runq_compare [--seconds 30]

#include <time.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>
#include <vector>

#include "collector/runq_histogram.h"
#include "collector/runq_probe.h"

namespace {

int64_t now_ns() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

}  // namespace

int main(int argc, char** argv) {
  int seconds = 30;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--seconds" && i + 1 < argc) {
      seconds = std::max(1, std::atoi(argv[++i]));
    } else {
      std::fprintf(stderr, "用法: runq_compare [--seconds N]\n");
      return 2;
    }
  }

  try {
    lm::worker::RunqProbe probe;
    lm::worker::RunqWindow window;
    lm::worker::RunqCumulative cum;

    // 单次读取耗时：读 200 次取中位数和最大值
    std::vector<int64_t> t;
    for (int i = 0; i < 200; ++i) {
      const int64_t a = now_ns();
      if (!probe.read(cum)) {
        std::fprintf(stderr, "读取 map 失败\n");
        return 1;
      }
      t.push_back(now_ns() - a);
    }
    std::sort(t.begin(), t.end());

    window.update(cum);  // 基线
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    if (!probe.read(cum)) {
      std::fprintf(stderr, "读取 map 失败\n");
      return 1;
    }
    auto st = window.update(cum);
    if (!st) {
      std::fprintf(stderr, "计数倒退，无法得到本周期分布\n");
      return 1;
    }

    std::printf("{\"seconds\":%d,\"samples\":%llu,\"drops\":%llu,\"p50_us\":%.2f,\"p99_us\":%.2f,"
                "\"read_us_p50\":%.1f,\"read_us_max\":%.1f,\"batch\":%s,\"buckets\":[",
                seconds, static_cast<unsigned long long>(st->samples),
                static_cast<unsigned long long>(st->drops), st->p50_us, st->p99_us,
                static_cast<double>(t[t.size() / 2]) / 1000.0,
                static_cast<double>(t.back()) / 1000.0, probe.batch_lookup() ? "true" : "false");
    const auto& d = window.last_delta();
    bool first = true;
    for (unsigned b = 0; b < RUNQ_BUCKETS; ++b) {
      if (d[b] == 0) continue;
      std::printf("%s[%llu,%llu,%llu]", first ? "" : ",",
                  static_cast<unsigned long long>(runq_bucket_low(b)),
                  static_cast<unsigned long long>(runq_bucket_width(b)),
                  static_cast<unsigned long long>(d[b]));
      first = false;
    }
    std::printf("]}\n");
  } catch (const std::exception& e) {
    std::fprintf(stderr, "%s\n", e.what());
    return 1;
  }
  return 0;
}
