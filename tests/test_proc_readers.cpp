#include <gtest/gtest.h>

#include <fstream>
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
