#include "proc_readers.h"

#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <ctime>
#include <stdexcept>
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

std::optional<double> parse_cpu_max(std::string_view text) {
  std::string t(text);
  char quota[32] = {0};
  unsigned long long period = 0;
  if (std::sscanf(t.c_str(), "%31s %llu", quota, &period) != 2 || period == 0) return std::nullopt;
  if (std::string_view(quota) == "max") return std::nullopt;
  char* end = nullptr;
  const double q = std::strtod(quota, &end);
  if (end == quota || !(q > 0)) return std::nullopt;
  return q / static_cast<double>(period);
}

unsigned parse_cpuset_count(std::string_view text) {
  unsigned n = 0;
  std::string t(text);
  while (!t.empty() && (t.back() == '\n' || t.back() == ' ')) t.pop_back();
  if (t.empty()) return 0;
  std::size_t pos = 0;
  while (pos <= t.size()) {
    const std::size_t comma = std::min(t.find(',', pos), t.size());
    const std::string part = t.substr(pos, comma - pos);
    unsigned a = 0;
    unsigned b = 0;
    if (std::sscanf(part.c_str(), "%u-%u", &a, &b) == 2) {
      if (b < a) return 0;
      n += b - a + 1;
    } else if (std::sscanf(part.c_str(), "%u", &a) == 1) {
      ++n;
    } else {
      return 0;
    }
    pos = comma + 1;
  }
  return n;
}

std::optional<uint64_t> parse_cpu_usage_usec(std::string_view text) {
  const std::string_view key = "usage_usec ";
  for (std::size_t pos = 0; pos < text.size();) {
    std::size_t eol = text.find('\n', pos);
    if (eol == std::string_view::npos) eol = text.size();
    const std::string_view line = text.substr(pos, eol - pos);
    if (line.substr(0, key.size()) == key) {
      return std::strtoull(std::string(line.substr(key.size())).c_str(), nullptr, 10);
    }
    pos = eol + 1;
  }
  return std::nullopt;
}

namespace {
bool read_small(const std::string& path, std::string& out) {
  ProcFile f(path);
  return f.read(out);
}
}  // namespace

CgroupCpuSource::CgroupCpuSource(const std::string& dir) : stat_(dir + "/cpu.stat") {
  std::string txt;
  if (!stat_.read(buf_) || !parse_cpu_usage_usec(buf_)) {
    throw std::runtime_error("读不到 " + dir + "/cpu.stat 的 usage_usec（需要 cgroup v2）");
  }
  std::optional<double> q;
  if (read_small(dir + "/cpu.max", txt)) q = parse_cpu_max(txt);
  if (!q) {
    unsigned c = 0;
    if (read_small(dir + "/cpuset.cpus.effective", txt)) c = parse_cpuset_count(txt);
    if (c == 0) {
      const long sc = ::sysconf(_SC_NPROCESSORS_ONLN);
      c = sc > 0 ? static_cast<unsigned>(sc) : 1;
    }
    q = static_cast<double>(c);
  }
  q_ = *q;
  n_ = std::max(1U, static_cast<unsigned>(std::ceil(q_ - 1e-9)));
}

bool CgroupCpuSource::read(CpuSnapshot& out) {
  if (!stat_.read(buf_)) return false;
  const auto usage = parse_cpu_usage_usec(buf_);
  if (!usage) return false;
  timespec ts{};
  ::clock_gettime(CLOCK_MONOTONIC, &ts);
  const uint64_t now_us = static_cast<uint64_t>(ts.tv_sec) * 1000000ULL +
                          static_cast<uint64_t>(ts.tv_nsec) / 1000ULL;
  const double busy_us = static_cast<double>(*usage) / q_;
  const uint64_t idle =
      busy_us >= static_cast<double>(now_us) ? 0 : now_us - static_cast<uint64_t>(busy_us);
  out.cores.clear();
  for (unsigned i = 0; i < n_; ++i) out.cores.push_back({i, idle, now_us});
  return true;
}

}  // namespace lm::worker
