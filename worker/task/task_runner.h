#pragma once

// 执行一个任务进程：posix_spawn、子进程自成进程组、stdout 与 stderr 合并到一根管道只保留末尾、
// 超时或取消时向整个进程组发 SIGKILL。纯 POSIX，macOS 与 Linux 都能编译和测试。

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace lm::worker {

struct RunSpec {
  std::string command;            // 绝对路径
  std::vector<std::string> args;  // 不含 argv[0]
  std::vector<std::string> env;   // "K=V"
  int64_t timeout_ms = 300 * 1000;
  std::size_t tail_bytes = 4096;
};

struct RunResult {
  // 正常退出为退出码；被信号杀死为 128 + 信号；超时 −1；无法启动 −2；被取消 −3。
  int exit_code = 0;
  std::string output_tail;
  uint32_t exec_ms = 0;
  bool timed_out = false;
  bool cancelled = false;
};

constexpr int kExitTimeout = -1;
constexpr int kExitSpawnFailed = -2;
constexpr int kExitCancelled = -3;

// 阻塞执行。cancelled 非空时每 50 ms 检查一次，返回 true 即杀进程组。
RunResult run_task(const RunSpec& spec, const std::function<bool()>& cancelled = {});

// 只保留最后 cap 字节的追加缓冲。
class TailBuffer {
 public:
  explicit TailBuffer(std::size_t cap) : cap_(cap) {}
  void append(const char* p, std::size_t n);
  const std::string& str() const { return buf_; }

 private:
  std::size_t cap_;
  std::string buf_;
};

}  // namespace lm::worker
