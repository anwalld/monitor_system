#pragma once

// /proc 与 /sys 读取。解析函数只接受文本，便于在没有 /proc 的机器上用 fixture 测试；
// Reader 类负责常开文件描述符、每次 pread 读取最新内容。

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>

#include "cpu_source.h"

namespace lm::worker {

// 解析 /proc/stat 的 cpuN 行（忽略汇总的 "cpu " 行）。
// idle = idle + iowait；total = user + nice + system + idle + iowait + irq + softirq + steal
// （guest 已计入 user，不重复加）。没有任何 cpuN 行时返回 false。
bool parse_proc_stat(std::string_view text, CpuSnapshot& out);

// 解析 /proc/meminfo，返回 MemAvailable / MemTotal，缺字段或 MemTotal 为 0 时返回空。
std::optional<double> parse_meminfo(std::string_view text);

// 解析 /proc/diskstats，返回 设备名 → io_ticks（第 13 列，累计忙碌毫秒）。
// 只保留 whole_disks 中的设备（整盘，排除分区）。
std::map<std::string, uint64_t> parse_diskstats(std::string_view text,
                                                const std::set<std::string>& whole_disks);

// 是否计入磁盘利用率：排除 loop、ram、zram、dm-、md、sr、nbd、fd 等虚拟或可移动设备。
bool is_physical_disk_name(std::string_view name);

// 常开文件、每次从头 pread 全部内容。
class ProcFile {
 public:
  explicit ProcFile(std::string path);
  ~ProcFile();
  ProcFile(const ProcFile&) = delete;
  ProcFile& operator=(const ProcFile&) = delete;

  bool read(std::string& out);
  const std::string& path() const { return path_; }

 private:
  std::string path_;
  int fd_ = -1;
};

class ProcCpuSource : public CpuSource {
 public:
  explicit ProcCpuSource(std::string path = "/proc/stat") : file_(std::move(path)) {}
  bool read(CpuSnapshot& out) override;

 private:
  ProcFile file_;
  std::string buf_;
};

class MemReader {
 public:
  explicit MemReader(std::string path = "/proc/meminfo") : file_(std::move(path)) {}
  std::optional<double> read_avail_ratio();

 private:
  ProcFile file_;
  std::string buf_;
};

// 磁盘利用率 = 两次读数之间 io_ticks 的增量 / 经过的毫秒数，取所有物理整盘的最大值。
class DiskReader {
 public:
  explicit DiskReader(std::string diskstats = "/proc/diskstats", std::string sys_block = "/sys/block");
  // now_ms 为单调时钟毫秒。首次调用只存基线并返回 0。读取失败返回空。
  std::optional<double> read_util(int64_t now_ms);

  // 测试用：直接给定整盘集合与文本。
  std::optional<double> update(const std::map<std::string, uint64_t>& ticks, int64_t now_ms);

 private:
  std::set<std::string> list_whole_disks() const;

  ProcFile file_;
  std::string sys_block_;
  std::string buf_;
  std::map<std::string, uint64_t> prev_ticks_;
  int64_t prev_ms_ = -1;
};

}  // namespace lm::worker
