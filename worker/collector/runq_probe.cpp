#include "runq_probe.h"

#include <stdexcept>
#include <string>

#if defined(LM_HAVE_BPF)

#include <bpf/bpf.h>
#include <bpf/libbpf.h>

#include <cerrno>
#include <cstdarg>
#include <cstdio>
#include <cstring>

#include <spdlog/spdlog.h>

#include "runqlat.skel.h"

namespace lm::worker {

namespace {

// libbpf 日志转到 spdlog 的 debug 级别；失败原因由异常消息给出，auto 模式下不刷屏
int libbpf_log(enum libbpf_print_level level, const char* fmt, va_list args) {
  if (level == LIBBPF_DEBUG) return 0;
  char buf[512];
  const int n = std::vsnprintf(buf, sizeof(buf), fmt, args);
  std::string line(buf, n > 0 ? std::min<std::size_t>(static_cast<std::size_t>(n), sizeof(buf) - 1) : 0);
  while (!line.empty() && line.back() == '\n') line.pop_back();
  spdlog::debug("libbpf: {}", line);
  return n;
}

std::runtime_error stage_error(const char* stage, int err) {
  if (err < 0) err = -err;
  return std::runtime_error(std::string("eBPF runqlat ") + stage + " 失败：" + std::strerror(err));
}

}  // namespace

struct RunqProbe::Impl {
  runqlat_bpf* skel = nullptr;
  int ncpu = 0;
  int hist_fd = -1;
  int drops_fd = -1;
  std::vector<uint32_t> keys;
  std::vector<uint64_t> values;  // RUNQ_BUCKETS × ncpu
};

RunqProbe::RunqProbe() : impl_(std::make_unique<Impl>()) {
  libbpf_set_print(libbpf_log);
  impl_->ncpu = libbpf_num_possible_cpus();
  if (impl_->ncpu <= 0) throw stage_error("num_possible_cpus", impl_->ncpu);

  impl_->skel = runqlat_bpf__open();
  if (impl_->skel == nullptr) throw stage_error("open", errno);
  int err = runqlat_bpf__load(impl_->skel);
  if (err != 0) {
    runqlat_bpf__destroy(impl_->skel);
    impl_->skel = nullptr;
    throw stage_error("load", err);
  }
  err = runqlat_bpf__attach(impl_->skel);
  if (err != 0) {
    runqlat_bpf__destroy(impl_->skel);
    impl_->skel = nullptr;
    throw stage_error("attach", err);
  }
  impl_->hist_fd = bpf_map__fd(impl_->skel->maps.hist);
  impl_->drops_fd = bpf_map__fd(impl_->skel->maps.drops);
  impl_->keys.resize(RUNQ_BUCKETS);
  impl_->values.resize(static_cast<std::size_t>(RUNQ_BUCKETS) * static_cast<std::size_t>(impl_->ncpu));
}

RunqProbe::~RunqProbe() {
  if (impl_ && impl_->skel) runqlat_bpf__destroy(impl_->skel);
}

bool RunqProbe::read(RunqCumulative& out) {
  const auto ncpu = static_cast<std::size_t>(impl_->ncpu);
  bool got = false;

  if (batch_ok_) {
    uint32_t out_batch = 0;
    uint32_t count = RUNQ_BUCKETS;
    LIBBPF_OPTS(bpf_map_batch_opts, opts);
    const int err = bpf_map_lookup_batch(impl_->hist_fd, nullptr, &out_batch, impl_->keys.data(),
                                         impl_->values.data(), &count, &opts);
    // 读到末尾时返回 -ENOENT，此时 count 仍有效
    const int e = errno;
    if (err != 0 && e != ENOENT) {
      if (e != EINVAL && e != ENOTSUP && e != EOPNOTSUPP) return false;
      spdlog::info("内核不支持 map 批量读取（{}），改为逐个键读取", std::strerror(e));
      batch_ok_ = false;
    } else {
      if (count != RUNQ_BUCKETS) return false;
      for (unsigned i = 0; i < RUNQ_BUCKETS; ++i) {
        const uint32_t key = impl_->keys[i];
        if (key >= RUNQ_BUCKETS) return false;
        uint64_t sum = 0;
        for (std::size_t c = 0; c < ncpu; ++c) sum += impl_->values[i * ncpu + c];
        out.buckets[key] = sum;
      }
      got = true;
    }
  }

  if (!got) {
    std::vector<uint64_t> per(ncpu);
    for (uint32_t key = 0; key < RUNQ_BUCKETS; ++key) {
      if (bpf_map_lookup_elem(impl_->hist_fd, &key, per.data()) != 0) return false;
      uint64_t sum = 0;
      for (uint64_t v : per) sum += v;
      out.buckets[key] = sum;
    }
  }

  std::vector<uint64_t> per(ncpu);
  const uint32_t zero = 0;
  if (bpf_map_lookup_elem(impl_->drops_fd, &zero, per.data()) != 0) return false;
  uint64_t d = 0;
  for (uint64_t v : per) d += v;
  out.drops = d;
  return true;
}

}  // namespace lm::worker

#else  // !LM_HAVE_BPF

namespace lm::worker {

struct RunqProbe::Impl {};

RunqProbe::RunqProbe() { throw std::runtime_error("构建时未启用 eBPF（LM_HAVE_BPF 未定义）"); }
RunqProbe::~RunqProbe() = default;
bool RunqProbe::read(RunqCumulative&) { return false; }

}  // namespace lm::worker

#endif
