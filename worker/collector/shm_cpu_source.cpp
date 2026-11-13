#include "shm_cpu_source.h"

#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/ioctl.h>
#endif

#include <cerrno>
#include <cstring>
#include <stdexcept>

#include "lm/seqlock_reader.h"

namespace lm::worker {

ShmCpuSource::ShmCpuSource(const std::string& path) : path_(path) {
  fd_ = ::open(path.c_str(), O_RDONLY | O_CLOEXEC);
  if (fd_ < 0) {
    throw std::runtime_error(path + ": " + std::strerror(errno));
  }
  void* p = ::mmap(nullptr, sizeof(cpustat_shm), PROT_READ, MAP_SHARED, fd_, 0);
  if (p == MAP_FAILED) {
    const int err = errno;
    ::close(fd_);
    fd_ = -1;
    throw std::runtime_error(path + ": mmap: " + std::strerror(err));
  }
  shm_ = static_cast<const cpustat_shm*>(p);
}

ShmCpuSource::~ShmCpuSource() {
  if (shm_) ::munmap(const_cast<cpustat_shm*>(shm_), sizeof(cpustat_shm));
  if (fd_ >= 0) ::close(fd_);
}

bool ShmCpuSource::read(CpuSnapshot& out) {
  uint32_t n = 0;
  if (!lm::read_cpustat(shm_, buf_, n) || n == 0) return false;
  out.cores.resize(n);
  for (uint32_t i = 0; i < n; ++i) {
    out.cores[i].cpu = i;
    out.cores[i].idle = buf_[i].idle_us + buf_[i].iowait_us;
    out.cores[i].total = buf_[i].stamp_us;
  }
  return true;
}

uint32_t ShmCpuSource::interval_us() const {
  return __atomic_load_n(&shm_->interval_us, __ATOMIC_RELAXED);
}

bool ShmCpuSource::set_interval_us(uint32_t us) {
#if defined(__linux__)
  return ::ioctl(fd_, CPUSTAT_SET_INTERVAL, &us) == 0;
#else
  (void)us;
  errno = ENOTSUP;
  return false;
#endif
}

}  // namespace lm::worker
