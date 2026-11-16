#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>
#include <sstream>

#include "collector/proc_readers.h"

using namespace lm::worker;

namespace {

std::string fixture(const std::string& name) {
  std::ifstream in(std::string(LM_FIXTURE_DIR) + "/" + name);
  std::stringstream ss;
  ss << in.rdbuf();
  return ss.str();
}

TEST(ProcStat, ParsesPerCpuLinesOnly) {
  CpuSnapshot s;
  ASSERT_TRUE(parse_proc_stat(fixture("proc_stat.txt"), s));
  ASSERT_EQ(s.cores.size(), 2U);
  // cpu0  4705 150 1120 16250 520 30 25 10 0 0
  EXPECT_EQ(s.cores[0].cpu, 0U);
  EXPECT_EQ(s.cores[0].idle, 16250U + 520U);
  EXPECT_EQ(s.cores[0].total, 4705U + 150 + 1120 + 16250 + 520 + 30 + 25 + 10);
  EXPECT_EQ(s.cores[1].cpu, 1U);
}

TEST(ProcStat, RejectsTextWithoutCpuLines) {
  CpuSnapshot s;
  EXPECT_FALSE(parse_proc_stat("intr 1 2 3\nctxt 5\n", s));
}

TEST(ProcStat, ReadsFromFileViaPread) {
  ProcCpuSource src(std::string(LM_FIXTURE_DIR) + "/proc_stat.txt");
  CpuSnapshot s;
  ASSERT_TRUE(src.read(s));
  ASSERT_TRUE(src.read(s));  // 重复读同一 fd
  EXPECT_EQ(s.cores.size(), 2U);
}

TEST(MemInfo, AvailableOverTotal) {
  auto r = parse_meminfo(fixture("meminfo.txt"));
  ASSERT_TRUE(r.has_value());
  EXPECT_NEAR(*r, 6000000.0 / 8000000.0, 1e-9);
  EXPECT_FALSE(parse_meminfo("MemTotal: 0 kB\nMemAvailable: 1 kB\n").has_value());
  EXPECT_FALSE(parse_meminfo("MemTotal: 100 kB\n").has_value());
}

TEST(DiskStats, FiltersToWholePhysicalDisks) {
  std::set<std::string> whole;
  for (const char* n : {"sda", "nvme0n1", "loop0", "dm-0", "sr0"}) {
    if (is_physical_disk_name(n)) whole.insert(n);
  }
  EXPECT_EQ(whole, (std::set<std::string>{"sda", "nvme0n1"}));
  auto ticks = parse_diskstats(fixture("diskstats.txt"), whole);
  ASSERT_EQ(ticks.size(), 2U);  // sda1 分区与 loop0 被排除
  EXPECT_EQ(ticks["sda"], 12000U);
  EXPECT_EQ(ticks["nvme0n1"], 3000U);
}

TEST(DiskReader, UtilIsMaxDeltaOverElapsed) {
  DiskReader r("/nonexistent", "/nonexistent");
  EXPECT_DOUBLE_EQ(*r.update({{"sda", 1000}, {"sdb", 0}}, 10000), 0.0);  // 基线
  // 1000ms 内 sda 忙 900ms、sdb 忙 200ms
  EXPECT_NEAR(*r.update({{"sda", 1900}, {"sdb", 200}}, 11000), 0.9, 1e-9);
  // 超过 100% 截断；新设备不参与
  EXPECT_DOUBLE_EQ(*r.update({{"sda", 4000}, {"sdb", 200}, {"sdc", 99999}}, 12000), 1.0);
}

}  // namespace

// ---------- M10：cgroup v2 CPU 来源 ----------

namespace {

using lm::worker::parse_cpu_max;
using lm::worker::parse_cpu_usage_usec;
using lm::worker::parse_cpuset_count;

TEST(CgroupCpu, ParseCpuMax) {
  EXPECT_FALSE(parse_cpu_max("max 100000\n"));
  EXPECT_DOUBLE_EQ(*parse_cpu_max("200000 100000\n"), 2.0);
  EXPECT_DOUBLE_EQ(*parse_cpu_max("150000 100000"), 1.5);
  EXPECT_FALSE(parse_cpu_max("garbage"));
  EXPECT_FALSE(parse_cpu_max("100000 0"));
}

TEST(CgroupCpu, ParseCpuset) {
  EXPECT_EQ(parse_cpuset_count("0-9\n"), 10U);
  EXPECT_EQ(parse_cpuset_count("0-3,6"), 5U);
  EXPECT_EQ(parse_cpuset_count("2"), 1U);
  EXPECT_EQ(parse_cpuset_count(""), 0U);
  EXPECT_EQ(parse_cpuset_count("3-1"), 0U);
}

TEST(CgroupCpu, ParseUsage) {
  EXPECT_EQ(*parse_cpu_usage_usec("usage_usec 5668355944\nuser_usec 1\n"), 5668355944ULL);
  EXPECT_FALSE(parse_cpu_usage_usec("user_usec 1\n"));
}

TEST(CgroupCpu, SourceFromDirectory) {
  const std::string dir = ::testing::TempDir() + "/lm_cg_test";
  std::filesystem::create_directories(dir);
  auto write = [&](const std::string& name, const std::string& v) {
    std::ofstream(dir + "/" + name) << v;
  };
  write("cpu.stat", "usage_usec 1000\n");
  write("cpu.max", "150000 100000\n");
  lm::worker::CgroupCpuSource src(dir);
  EXPECT_DOUBLE_EQ(src.quota_cores(), 1.5);
  EXPECT_EQ(src.cores(), 2U);
  lm::worker::CpuSnapshot a;
  ASSERT_TRUE(src.read(a));
  ASSERT_EQ(a.cores.size(), 2U);
  EXPECT_EQ(a.cores[0].total, a.cores[1].total);
  EXPECT_EQ(a.cores[0].total - a.cores[0].idle, 666U);  // 1000 / 1.5
  write("cpu.max", "max 100000\n");
  write("cpuset.cpus.effective", "0-3\n");
  lm::worker::CgroupCpuSource src2(dir);
  EXPECT_EQ(src2.cores(), 4U);
  EXPECT_THROW(lm::worker::CgroupCpuSource(dir + "/missing"), std::runtime_error);
  // 忙碌度：两次读数间 usage 增加 = 经过时间 × q 时为 1
  write("cpu.max", "200000 100000\n");
  lm::worker::CgroupCpuSource src3(dir);
  lm::worker::CpuSnapshot s1, s2;
  ASSERT_TRUE(src3.read(s1));
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  ASSERT_TRUE(src3.read(s2));
  const uint64_t dt = s2.cores[0].total - s1.cores[0].total;
  write("cpu.stat", "usage_usec " + std::to_string(1000 + 2 * dt * 2) + "\n");  // 远超可用 → 截断为 1
  lm::worker::CpuSnapshot s3;
  ASSERT_TRUE(src3.read(s3));
  lm::worker::CpuBusyCalculator calc;
  std::vector<float> core;
  float busy = 0;
  calc.update(s2, core, busy);
  ASSERT_EQ(calc.update(s3, core, busy), lm::worker::BusyResult::kUpdated);
  EXPECT_GT(busy, 0.9F);
  std::filesystem::remove_all(dir);
}

}  // namespace
