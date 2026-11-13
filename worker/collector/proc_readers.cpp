#include "proc_readers.h"

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <vector>

namespace lm::worker {

namespace {

// 按空白切分一行
std::vector<std::string_view> split_ws(std::string_view line) {
  std::vector<std::string_view> out;
  std::size_t i = 0;
  while (i < line.size()) {
    while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
    std::size_t j = i;
    while (j < line.size() && line[j] != ' ' && line[j] != '\t') ++j;
    if (j > i) out.push_back(line.substr(i, j - i));
    i = j;
  }
  return out;
}

bool to_u64(std::string_view s, uint64_t& v) {
  if (s.empty()) return false;
  uint64_t r = 0;
  for (char c : s) {
    if (c < '0' || c > '9') return false;
    r = r * 10 + static_cast<uint64_t>(c - '0');
  }
  v = r;
  return true;
}

template <typename F>
void for_each_line(std::string_view text, F&& fn) {
  std::size_t start = 0;
  while (start < text.size()) {
    std::size_t end = text.find('\n', start);
    if (end == std::string_view::npos) end = text.size();
    fn(text.substr(start, end - start));
    start = end + 1;
  }
}

}  // namespace

bool parse_proc_stat(std::string_view text, CpuSnapshot& out) {
  out.cores.clear();
  for_each_line(text, [&](std::string_view line) {
    if (line.size() < 4 || line.substr(0, 3) != "cpu" || line[3] < '0' || line[3] > '9') return;
    auto f = split_ws(line);
    if (f.size() < 9) return;
    uint64_t id = 0;
    if (!to_u64(f[0].substr(3), id)) return;
    uint64_t v[8] = {};  // user nice system idle iowait irq softirq steal
    for (int k = 0; k < 8; ++k) {
      if (!to_u64(f[1 + k], v[k])) return;
    }
    CoreTimes c;
    c.cpu = static_cast<uint32_t>(id);
    c.idle = v[3] + v[4];
    c.total = v[0] + v[1] + v[2] + v[3] + v[4] + v[5] + v[6] + v[7];
    out.cores.push_back(c);
  });
  std::sort(out.cores.begin(), out.cores.end(),
            [](const CoreTimes& a, const CoreTimes& b) { return a.cpu < b.cpu; });
  return !out.cores.empty();
}

std::optional<double> parse_meminfo(std::string_view text) {
  uint64_t total = 0, avail = 0;
  bool has_total = false, has_avail = false;
  for_each_line(text, [&](std::string_view line) {
    auto f = split_ws(line);
    if (f.size() < 2) return;
    if (f[0] == "MemTotal:") has_total = to_u64(f[1], total);
    if (f[0] == "MemAvailable:") has_avail = to_u64(f[1], avail);
  });
  if (!has_total || !has_avail || total == 0) return std::nullopt;
  return std::clamp(static_cast<double>(avail) / static_cast<double>(total), 0.0, 1.0);
}

std::map<std::string, uint64_t> parse_diskstats(std::string_view text,
                                                const std::set<std::string>& whole_disks) {
  std::map<std::string, uint64_t> out;
  for_each_line(text, [&](std::string_view line) {
    auto f = split_ws(line);
    // major minor name 后依次为 11 个（或更多）统计列，io_ticks 是第 13 列（下标 12）
    if (f.size() < 13) return;
    std::string name(f[2]);
    if (whole_disks.count(name) == 0) return;
    uint64_t ticks = 0;
    if (!to_u64(f[12], ticks)) return;
    out[name] = ticks;
  });
  return out;
}

bool is_physical_disk_name(std::string_view name) {
  static constexpr std::string_view kExcluded[] = {"loop", "ram", "zram", "dm-", "md",
                                                   "sr",   "nbd", "fd"};
  for (auto p : kExcluded) {
    if (name.substr(0, p.size()) == p) return false;
  }
  return !name.empty();
}

// ---------------- ProcFile ----------------

ProcFile::ProcFile(std::string path) : path_(std::move(path)) {
  fd_ = ::open(path_.c_str(), O_RDONLY | O_CLOEXEC);
}

ProcFile::~ProcFile() {
  if (fd_ >= 0) ::close(fd_);
}

bool ProcFile::read(std::string& out) {
  if (fd_ < 0) {
    fd_ = ::open(path_.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd_ < 0) return false;
  }
  out.clear();
  char buf[8192];
  off_t off = 0;
  for (;;) {
    const ssize_t n = ::pread(fd_, buf, sizeof(buf), off);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (n == 0) break;
    out.append(buf, static_cast<std::size_t>(n));
    off += n;
  }
  return !out.empty();
}

// ---------------- Readers ----------------

bool ProcCpuSource::read(CpuSnapshot& out) {
  if (!file_.read(buf_)) return false;
  return parse_proc_stat(buf_, out);
}

std::optional<double> MemReader::read_avail_ratio() {
  if (!file_.read(buf_)) return std::nullopt;
  return parse_meminfo(buf_);
}

DiskReader::DiskReader(std::string diskstats, std::string sys_block)
    : file_(std::move(diskstats)), sys_block_(std::move(sys_block)) {}

std::set<std::string> DiskReader::list_whole_disks() const {
  std::set<std::string> out;
  DIR* d = ::opendir(sys_block_.c_str());
  if (d == nullptr) return out;
  while (dirent* e = ::readdir(d)) {
    std::string_view name(e->d_name);
    if (name == "." || name == "..") continue;
    if (is_physical_disk_name(name)) out.emplace(name);
  }
  ::closedir(d);
  return out;
}

std::optional<double> DiskReader::read_util(int64_t now_ms) {
  if (!file_.read(buf_)) return std::nullopt;
  return update(parse_diskstats(buf_, list_whole_disks()), now_ms);
}

std::optional<double> DiskReader::update(const std::map<std::string, uint64_t>& ticks,
                                         int64_t now_ms) {
  double best = 0.0;
  if (prev_ms_ >= 0 && now_ms > prev_ms_) {
    const double dt = static_cast<double>(now_ms - prev_ms_);
    for (const auto& [name, t] : ticks) {
      auto it = prev_ticks_.find(name);
      if (it == prev_ticks_.end() || t < it->second) continue;  // 新设备或计数重置
      best = std::max(best, static_cast<double>(t - it->second) / dt);
    }
  }
  prev_ticks_ = ticks;
  prev_ms_ = now_ms;
  return std::clamp(best, 0.0, 1.0);
}

}  // namespace lm::worker
