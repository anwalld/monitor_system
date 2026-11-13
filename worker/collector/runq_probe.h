#pragma once

// 加载 runqlat eBPF 程序并读取各核累计直方图（M3）。
// 构建时未启用 eBPF（LM_HAVE_BPF 未定义，如 macOS）时，构造函数直接抛出异常。

#include <cstdint>
#include <memory>
#include <vector>

#include "runq_histogram.h"

namespace lm::worker {

class RunqProbe {
 public:
  // 打开、加载、挂载；失败抛 std::runtime_error，消息注明阶段（open/load/attach）和原因。
  RunqProbe();
  ~RunqProbe();
  RunqProbe(const RunqProbe&) = delete;
  RunqProbe& operator=(const RunqProbe&) = delete;

  // 读取各核求和后的累计值。失败返回 false。
  bool read(RunqCumulative& out);

  // 是否在用批量读取（内核不支持时退回逐个键读取）。
  bool batch_lookup() const { return batch_ok_; }

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
  bool batch_ok_ = true;
};

}  // namespace lm::worker
