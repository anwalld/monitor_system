// TaskRunner：posix_spawn、进程组、输出末尾、超时、取消（macOS 与 Linux 都跑）。

#include <gtest/gtest.h>

#include <signal.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <thread>

#include "task_runner.h"

using namespace lm::worker;

namespace {

RunSpec sh(const std::string& script, int64_t timeout_ms = 10000) {
  RunSpec s;
  s.command = "/bin/sh";
  s.args = {"-c", script};
  s.env = {"PATH=/usr/bin:/bin", "LM_TASK_ID=t1"};
  s.timeout_ms = timeout_ms;
  return s;
}

TEST(TailBuffer, KeepsLastBytes) {
  TailBuffer b(4);
  b.append("ab", 2);
  b.append("cde", 3);
  EXPECT_EQ(b.str(), "bcde");
  b.append("0123456789", 10);
  EXPECT_EQ(b.str(), "6789");
}

TEST(TaskRunner, ExitCodeAndMergedOutput) {
  const auto r = run_task(sh("echo hi; echo err >&2; exit 3"));
  EXPECT_EQ(r.exit_code, 3);
  EXPECT_EQ(r.output_tail, "hi\nerr\n");
  EXPECT_FALSE(r.timed_out);
}

TEST(TaskRunner, EnvironmentIsMinimal) {
  const auto r = run_task(sh("echo $LM_TASK_ID:$HOME"));
  EXPECT_EQ(r.exit_code, 0);
  EXPECT_EQ(r.output_tail, "t1:\n");
}

TEST(TaskRunner, KilledBySignal) {
  const auto r = run_task(sh("kill -TERM $$"));
  EXPECT_EQ(r.exit_code, 128 + SIGTERM);
}

TEST(TaskRunner, TimeoutKillsWholeGroup) {
  const std::string pidfile = "/tmp/lmtest_task_runner_" + std::to_string(::getpid());
  const auto t0 = std::chrono::steady_clock::now();
  const auto r = run_task(sh("sleep 100 & echo $! > " + pidfile + "; sleep 100", 1000));
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0).count();
  EXPECT_EQ(r.exit_code, kExitTimeout);
  EXPECT_TRUE(r.timed_out);
  EXPECT_GE(ms, 950);
  EXPECT_LT(ms, 3000);
  std::ifstream f(pidfile);
  pid_t bg = 0;
  f >> bg;
  ASSERT_GT(bg, 0);
  bool gone = false;
  for (int i = 0; i < 50 && !gone; ++i) {
    gone = ::kill(bg, 0) != 0;
    if (!gone) std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  EXPECT_TRUE(gone) << "后台 sleep 仍在运行";
  ::unlink(pidfile.c_str());
}

TEST(TaskRunner, BackgroundChildDoesNotHoldTask) {
  const auto t0 = std::chrono::steady_clock::now();
  const auto r = run_task(sh("sleep 100 & echo started", 5000));
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - t0).count();
  EXPECT_EQ(r.exit_code, 0);
  EXPECT_EQ(r.output_tail, "started\n");
  EXPECT_LT(ms, 1500);
}

TEST(TaskRunner, OutputTruncatedToTail) {
  RunSpec s = sh("i=0; while [ $i -lt 1000 ]; do printf '%09d\\n' $i; i=$((i+1)); done");
  s.tail_bytes = 4096;
  const auto r = run_task(s);
  EXPECT_EQ(r.exit_code, 0);
  ASSERT_EQ(r.output_tail.size(), 4096U);
  EXPECT_EQ(r.output_tail.substr(r.output_tail.size() - 10), "000000999\n");
}

TEST(TaskRunner, MissingCommand) {
  RunSpec s;
  s.command = "/nonexistent/lm-task";
  const auto r = run_task(s);
  EXPECT_EQ(r.exit_code, kExitSpawnFailed);
  EXPECT_NE(r.output_tail.find("/nonexistent/lm-task"), std::string::npos) << r.output_tail;
}

TEST(TaskRunner, Cancel) {
  std::atomic<bool> cancel{false};
  std::thread t([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    cancel.store(true);
  });
  const auto r = run_task(sh("sleep 100"), [&] { return cancel.load(); });
  t.join();
  EXPECT_EQ(r.exit_code, kExitCancelled);
  EXPECT_TRUE(r.cancelled);
  EXPECT_LT(r.exec_ms, 2000U);
}

}  // namespace
