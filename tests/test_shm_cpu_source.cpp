#include <gtest/gtest.h>

#include <fcntl.h>
#include <unistd.h>

#include <chrono>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

#include "collector/interval_guard.h"
#include "collector/proc_readers.h"
#include "collector/shm_cpu_source.h"

using namespace lm::worker;

namespace {

// 用普通文件模拟共享页：写一份 cpustat_shm，ShmCpuSource 对它 mmap。
class FakeShmFile {
 public:
  FakeShmFile() {
    char tmpl[] = "/tmp/lm_cpustat_XXXXXX";
    fd_ = ::mkstemp(tmpl);
    path_ = tmpl;
  }
  ~FakeShmFile() {
    ::close(fd_);
    ::unlink(path_.c_str());
  }
  void write(const cpustat_shm& s) {
    ASSERT_EQ(::pwrite(fd_, &s, sizeof(s), 0), static_cast<ssize_t>(sizeof(s)));
  }
  const std::string& path() const { return path_; }

 private:
  int fd_ = -1;
  std::string path_;
};

TEST(ShmCpuSource, MissingDeviceThrowsWithReason) {
  try {
    ShmCpuSource src("/nonexistent/cpustat");
    FAIL() << "expected exception";
  } catch (const std::runtime_error& e) {
    EXPECT_NE(std::string(e.what()).find("/nonexistent/cpustat"), std::string::npos);
  }
}

TEST(ShmCpuSource, ConvertsSharedPageToSnapshot) {
  FakeShmFile f;
  auto s = std::make_unique<cpustat_shm>();
  s->seq = 10;
  s->ncpu = 2;
  s->interval_us = 1000000;
  s->cpu[0] = {700, 100, 5000};
  s->cpu[1] = {900, 50, 5001};
  f.write(*s);

  ShmCpuSource src(f.path());
  CpuSnapshot snap;
  ASSERT_TRUE(src.read(snap));
  ASSERT_EQ(snap.cores.size(), 2U);
  EXPECT_EQ(snap.cores[0].cpu, 0U);
  EXPECT_EQ(snap.cores[0].idle, 800U);
  EXPECT_EQ(snap.cores[0].total, 5000U);
  EXPECT_EQ(snap.cores[1].idle, 950U);
  EXPECT_EQ(snap.cores[1].total, 5001U);
  EXPECT_EQ(src.interval_us(), 1000000U);
}

TEST(ShmCpuSource, OddSeqFailsRead) {
  FakeShmFile f;
  auto s = std::make_unique<cpustat_shm>();
  s->seq = 3;
  s->ncpu = 1;
  f.write(*s);
  ShmCpuSource src(f.path());
  CpuSnapshot snap;
  EXPECT_FALSE(src.read(snap));
}

TEST(IntervalGuard, NoOpForNonShmSource) {
  ProcCpuSource proc("/nonexistent/stat");
  IntervalGuard g(&proc, 1000);
  EXPECT_FALSE(g.active());
  IntervalGuard g2(nullptr, 1000);
  EXPECT_FALSE(g2.active());
}

TEST(IntervalGuard, IoctlFailureIsOnlyAWarning) {
  FakeShmFile f;
  auto s = std::make_unique<cpustat_shm>();
  s->seq = 2;
  s->ncpu = 1;
  s->interval_us = 1000000;
  f.write(*s);
  ShmCpuSource src(f.path());
  IntervalGuard g(&src, 1000);  // 普通文件不支持 ioctl
  EXPECT_FALSE(g.active());
}

// 以下用例需要已加载 cpustat 模块且当前用户能打开 /dev/cpustat（Linux 验收时运行）。
uint32_t wait_interval(const ShmCpuSource& src, uint32_t want) {
  for (int i = 0; i < 100 && src.interval_us() != want; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return src.interval_us();
}

TEST(IntervalGuardLive, SetsAndRestoresIncludingOnException) {
  if (::access(CPUSTAT_DEV_PATH, R_OK) != 0) GTEST_SKIP() << "no readable " CPUSTAT_DEV_PATH;
  ShmCpuSource src;
  const uint32_t before = src.interval_us();
  {
    IntervalGuard g(&src, 1000);
    ASSERT_TRUE(g.active());
    EXPECT_EQ(wait_interval(src, 1000), 1000U);
  }
  EXPECT_EQ(wait_interval(src, before), before);

  try {
    IntervalGuard g(&src, 1000);
    EXPECT_EQ(wait_interval(src, 1000), 1000U);
    throw std::runtime_error("boom");
  } catch (const std::runtime_error&) {
  }
  EXPECT_EQ(wait_interval(src, before), before);

  CpuSnapshot snap;
  EXPECT_TRUE(src.read(snap));
  EXPECT_GE(snap.cores.size(), 1U);
}

}  // namespace
