#include <gtest/gtest.h>

#include "collector/cpu_source.h"

using lm::worker::BusyResult;
using lm::worker::CoreTimes;
using lm::worker::CpuBusyCalculator;
using lm::worker::CpuSnapshot;

namespace {

CpuSnapshot snap(std::initializer_list<CoreTimes> c) { return CpuSnapshot{c}; }

TEST(CpuBusy, FirstUpdateIsBaselineOnly) {
  CpuBusyCalculator calc;
  std::vector<float> cores;
  float all = -1;
  EXPECT_EQ(calc.update(snap({{0, 100, 1000}, {1, 100, 1000}}), cores, all), BusyResult::kBaseline);
  EXPECT_TRUE(cores.empty());
}

TEST(CpuBusy, ComputesPerCoreAndAverage) {
  CpuBusyCalculator calc;
  std::vector<float> cores;
  float all = 0;
  calc.update(snap({{0, 0, 0}, {1, 0, 0}, {2, 0, 0}, {3, 0, 0}}), cores, all);
  // core0 全忙，core1 一半，core2/3 空闲
  ASSERT_EQ(calc.update(snap({{0, 0, 100}, {1, 50, 100}, {2, 100, 100}, {3, 100, 100}}), cores, all), BusyResult::kUpdated);
  ASSERT_EQ(cores.size(), 4U);
  EXPECT_FLOAT_EQ(cores[0], 1.0F);
  EXPECT_FLOAT_EQ(cores[1], 0.5F);
  EXPECT_FLOAT_EQ(cores[2], 0.0F);
  EXPECT_FLOAT_EQ(all, 0.375F);
}

TEST(CpuBusy, ZeroDeltaAndCounterResetAreZeroNotNegative) {
  CpuBusyCalculator calc;
  std::vector<float> cores;
  float all = 0;
  calc.update(snap({{0, 500, 1000}, {1, 500, 1000}}), cores, all);
  // core0 时间没走；core1 计数器倒退
  ASSERT_EQ(calc.update(snap({{0, 500, 1000}, {1, 10, 20}}), cores, all), BusyResult::kUpdated);
  EXPECT_FLOAT_EQ(cores[0], 0.0F);
  EXPECT_FLOAT_EQ(cores[1], 0.0F);
}

TEST(CpuBusy, IdleGrowingFasterThanTotalClampsToZero) {
  CpuBusyCalculator calc;
  std::vector<float> cores;
  float all = 0;
  calc.update(snap({{0, 0, 0}}), cores, all);
  ASSERT_EQ(calc.update(snap({{0, 120, 100}}), cores, all), BusyResult::kUpdated);
  EXPECT_FLOAT_EQ(cores[0], 0.0F);
}

TEST(CpuBusy, CpuSetChangeResetsBaseline) {
  CpuBusyCalculator calc;
  std::vector<float> cores;
  float all = 0;
  calc.update(snap({{0, 0, 0}, {1, 0, 0}}), cores, all);
  EXPECT_EQ(calc.update(snap({{0, 0, 100}}), cores, all), BusyResult::kBaseline);  // CPU1 下线
  EXPECT_EQ(calc.update(snap({{0, 0, 200}}), cores, all), BusyResult::kUpdated);
}

TEST(CpuBusy, SameSnapshotIsNoNewDataAndKeepsBaseline) {
  CpuBusyCalculator calc;
  std::vector<float> cores;
  float all = 0;
  calc.update(snap({{0, 0, 0}, {1, 0, 0}}), cores, all);
  ASSERT_EQ(calc.update(snap({{0, 50, 100}, {1, 100, 100}}), cores, all), BusyResult::kUpdated);
  // 读到同一次刷新：不算、不动基线
  EXPECT_EQ(calc.update(snap({{0, 50, 100}, {1, 100, 100}}), cores, all), BusyResult::kNoNewData);
  // 下一次从基线 (50,100)/(100,100) 起算：core0 Δtotal=200、Δidle=0 → 1.0；core1 Δidle=200 → 0
  ASSERT_EQ(calc.update(snap({{0, 50, 300}, {1, 300, 300}}), cores, all), BusyResult::kUpdated);
  EXPECT_FLOAT_EQ(cores[0], 1.0F);
  EXPECT_FLOAT_EQ(cores[1], 0.0F);
}

}  // namespace
