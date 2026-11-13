#include "traffic.h"

#include <fcntl.h>
#include <unistd.h>

#include <cstdlib>

namespace lm::worker {

uint64_t RateCounter::update(uint64_t bytes, int64_t now_ns) {
  if (!has_prev_ || bytes < prev_bytes_ || now_ns <= prev_ns_) {
    has_prev_ = true;
    prev_bytes_ = bytes;
    prev_ns_ = now_ns;
    return 0;
  }
  const long double bits = static_cast<long double>(bytes - prev_bytes_) * 8.0L;
  const long double secs = static_cast<long double>(now_ns - prev_ns_) / 1e9L;
  prev_bytes_ = bytes;
  prev_ns_ = now_ns;
  return static_cast<uint64_t>(bits / secs + 0.5L);
}

SysfsCounter::SysfsCounter(const std::string& path) {
  fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
}

SysfsCounter::~SysfsCounter() {
  if (fd_ >= 0) ::close(fd_);
}

std::optional<uint64_t> SysfsCounter::read() const {
  if (fd_ < 0) return std::nullopt;
  char buf[32];
  const ssize_t n = ::pread(fd_, buf, sizeof(buf) - 1, 0);
  if (n <= 0) return std::nullopt;
  buf[n] = '\0';
  char* end = nullptr;
  const unsigned long long v = std::strtoull(buf, &end, 10);
  if (end == buf) return std::nullopt;
  return static_cast<uint64_t>(v);
}

}  // namespace lm::worker
