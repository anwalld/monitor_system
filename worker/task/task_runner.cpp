#include "task_runner.h"

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <algorithm>
#include <chrono>
#include <cstring>

namespace lm::worker {

void TailBuffer::append(const char* p, std::size_t n) {
  if (cap_ == 0) return;
  if (n >= cap_) {
    buf_.assign(p + (n - cap_), cap_);
    return;
  }
  buf_.append(p, n);
  if (buf_.size() > cap_) buf_.erase(0, buf_.size() - cap_);
}

namespace {

int64_t now_ms() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

int decode_status(int status) {
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
  return kExitSpawnFailed;
}

}  // namespace

RunResult run_task(const RunSpec& spec, const std::function<bool()>& cancelled) {
  RunResult res;
  TailBuffer tail(spec.tail_bytes);

  int fds[2];
  if (::pipe(fds) != 0) {
    res.exit_code = kExitSpawnFailed;
    res.output_tail = std::string("创建管道失败: ") + std::strerror(errno);
    return res;
  }
  ::fcntl(fds[0], F_SETFD, FD_CLOEXEC);

  posix_spawn_file_actions_t fa;
  posix_spawn_file_actions_init(&fa);
  posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
  posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
  posix_spawn_file_actions_adddup2(&fa, fds[1], 2);
  posix_spawn_file_actions_addclose(&fa, fds[1]);

  posix_spawnattr_t attr;
  posix_spawnattr_init(&attr);
  sigset_t empty;
  sigemptyset(&empty);
  sigset_t defaults;
  sigemptyset(&defaults);
  sigaddset(&defaults, SIGPIPE);
  sigaddset(&defaults, SIGINT);
  sigaddset(&defaults, SIGTERM);
  posix_spawnattr_setsigmask(&attr, &empty);
  posix_spawnattr_setsigdefault(&attr, &defaults);
  posix_spawnattr_setpgroup(&attr, 0);
  posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK |
                                      POSIX_SPAWN_SETSIGDEF);

  std::vector<char*> argv;
  argv.push_back(const_cast<char*>(spec.command.c_str()));
  for (const auto& a : spec.args) argv.push_back(const_cast<char*>(a.c_str()));
  argv.push_back(nullptr);
  std::vector<char*> envp;
  for (const auto& e : spec.env) envp.push_back(const_cast<char*>(e.c_str()));
  envp.push_back(nullptr);

  const int64_t start = now_ms();
  pid_t pid = -1;
  const int rc = ::posix_spawn(&pid, spec.command.c_str(), &fa, &attr, argv.data(), envp.data());
  posix_spawn_file_actions_destroy(&fa);
  posix_spawnattr_destroy(&attr);
  ::close(fds[1]);
  if (rc != 0) {
    ::close(fds[0]);
    res.exit_code = kExitSpawnFailed;
    res.output_tail = "无法启动 " + spec.command + ": " + std::strerror(rc);
    return res;
  }

  const int64_t deadline = start + spec.timeout_ms;
  bool killed = false;
  auto kill_group = [&] {
    if (!killed) {
      ::kill(-pid, SIGKILL);
      killed = true;
    }
  };
  char buf[4096];
  bool eof = false;
  bool reaped = false;
  int status = 0;
  int64_t reaped_at = 0;
  constexpr int64_t kDrainGraceMs = 100;  // 主进程退出后再等多久收尾输出
  while (!eof) {
    const int64_t now = now_ms();
    if (!reaped) {
      const pid_t w = ::waitpid(pid, &status, WNOHANG);
      if (w == pid) {
        reaped = true;
        reaped_at = now;
        res.exec_ms = static_cast<uint32_t>(now - start);
      }
    }
    // 主进程已退出但孙进程还占着输出（例如后台进程）：短暂收尾后整组杀掉，任务以主进程为准
    if (reaped && now - reaped_at >= kDrainGraceMs) {
      ::kill(-pid, SIGKILL);
      break;
    }
    if (!reaped && !killed && now >= deadline) {
      res.timed_out = true;
      kill_group();
    }
    if (!reaped && !killed && cancelled && cancelled()) {
      res.cancelled = true;
      kill_group();
    }
    int wait_ms = 20;
    if (!killed && !reaped) {
      wait_ms = static_cast<int>(std::min<int64_t>(20, std::max<int64_t>(0, deadline - now)));
    }
    pollfd p{fds[0], POLLIN, 0};
    const int pr = ::poll(&p, 1, wait_ms);
    if (pr < 0) {
      if (errno == EINTR) continue;
      break;
    }
    if (pr == 0) continue;
    const ssize_t n = ::read(fds[0], buf, sizeof(buf));
    if (n > 0) {
      tail.append(buf, static_cast<std::size_t>(n));
    } else if (n == 0) {
      eof = true;  // 所有写端（含孙进程）都已关闭
    } else if (errno != EINTR && errno != EAGAIN) {
      break;
    }
  }
  ::close(fds[0]);
  if (!reaped) {
    while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    res.exec_ms = static_cast<uint32_t>(now_ms() - start);
  }
  // 超时或取消时，主进程退出后孙进程可能仍在：整组再杀一次
  if (killed) ::kill(-pid, SIGKILL);
  res.output_tail = tail.str();
  if (res.timed_out) {
    res.exit_code = kExitTimeout;
  } else if (res.cancelled) {
    res.exit_code = kExitCancelled;
  } else {
    res.exit_code = decode_status(status);
  }
  return res;
}

}  // namespace lm::worker
