#include "prober.h"

#include <stdexcept>
#include <string>

#if defined(__linux__)

#include <linux/icmp.h>  // ICMP_FILTER
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <thread>

#include <spdlog/spdlog.h>

#include "icmp.h"

namespace lm::worker {

namespace {

int64_t mono_ns() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

constexpr int kTickMs = 100;

}  // namespace

struct Prober::Impl {
  struct Target {
    uint32_t addr;
    int64_t next_ns;
  };
  struct Nic {
    std::string name;
    int fd = -1;
    uint16_t seq = 0;
    std::vector<Target> targets;
    ProbeState state;
    explicit Nic(int64_t timeout_ns) : state(timeout_ns) {}
  };

  std::vector<Nic> nics;
  int epfd = -1;
  int timerfd = -1;
  int stopfd = -1;
  int64_t interval_ns = 0;
  uint16_t ident = 0;
  std::mutex mu;  // 保护各 Nic::state（探测线程记账，采集线程 take）
  std::thread thread;
  std::atomic<bool> running{false};

  ~Impl() {
    for (auto& n : nics) {
      if (n.fd >= 0) ::close(n.fd);
    }
    for (int fd : {epfd, timerfd, stopfd}) {
      if (fd >= 0) ::close(fd);
    }
  }

  void send_due(Nic& n, uint32_t index, int64_t now) {
    for (auto& t : n.targets) {
      if (now < t.next_ns) continue;
      t.next_ns += interval_ns;
      if (t.next_ns <= now) t.next_ns = now + interval_ns;  // 落后太多时对齐，不补发
      const uint16_t seq = n.seq++;
      const auto pkt = build_echo_request(ident, seq, index, static_cast<uint64_t>(now));
      sockaddr_in dst{};
      dst.sin_family = AF_INET;
      dst.sin_addr.s_addr = t.addr;
      const ssize_t r = ::sendto(n.fd, pkt.data(), pkt.size(), 0,
                                 reinterpret_cast<sockaddr*>(&dst), sizeof(dst));
      std::lock_guard<std::mutex> lk(mu);
      if (r == static_cast<ssize_t>(pkt.size())) {
        n.state.on_sent(seq, t.addr, now);
      } else {
        n.state.on_send_failed();
      }
    }
  }

  void drain(uint32_t index) {
    Nic& n = nics[index];
    uint8_t buf[1500];
    for (;;) {
      const ssize_t r = ::recv(n.fd, buf, sizeof(buf), 0);
      if (r < 0) break;  // EAGAIN 或出错
      const int64_t now = mono_ns();
      EchoReply rep;
      if (!parse_echo_reply(buf, static_cast<std::size_t>(r), rep)) continue;
      if (rep.ident != ident || rep.nic_index != index) continue;
      std::lock_guard<std::mutex> lk(mu);
      n.state.on_reply(rep.seq, rep.src, now);
    }
  }

  void loop() {
    epoll_event evs[16];
    while (running.load(std::memory_order_relaxed)) {
      const int k = ::epoll_wait(epfd, evs, 16, -1);
      if (k < 0) {
        if (errno == EINTR) continue;
        spdlog::error("网卡探测 epoll_wait 失败：{}", std::strerror(errno));
        break;
      }
      for (int i = 0; i < k; ++i) {
        const uint32_t id = evs[i].data.u32;
        if (id == nics.size() + 1) return;  // stopfd
        if (id == nics.size()) {            // timerfd
          uint64_t exp = 0;
          (void)!::read(timerfd, &exp, sizeof(exp));
          const int64_t now = mono_ns();
          for (uint32_t j = 0; j < nics.size(); ++j) {
            {
              std::lock_guard<std::mutex> lk(mu);
              nics[j].state.expire(now);
            }
            send_due(nics[j], j, now);
          }
        } else {
          drain(id);
        }
      }
    }
  }
};

Prober::Prober(const NetProbeConfig& cfg, const std::vector<NicSpec>& specs)
    : impl_(std::make_unique<Impl>()) {
  auto& im = *impl_;
  im.interval_ns = static_cast<int64_t>(cfg.interval_ms) * 1000000LL;
  im.ident = static_cast<uint16_t>(::getpid() & 0xffff);
  const int64_t timeout_ns = static_cast<int64_t>(cfg.timeout_ms) * 1000000LL;
  const int64_t now = mono_ns();

  im.nics.reserve(specs.size());
  for (const auto& s : specs) {
    Impl::Nic n(timeout_ns);
    n.name = s.name;
    n.fd = ::socket(AF_INET, SOCK_RAW | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_ICMP);
    if (n.fd < 0) throw std::runtime_error(s.name + ": 创建原始 ICMP socket 失败：" + std::strerror(errno));
    if (::setsockopt(n.fd, SOL_SOCKET, SO_BINDTODEVICE, s.name.c_str(),
                     static_cast<socklen_t>(s.name.size())) != 0) {
      const int e = errno;
      ::close(n.fd);
      throw std::runtime_error(s.name + ": SO_BINDTODEVICE 失败：" + std::strerror(e));
    }
    icmp_filter filt{};
    filt.data = ~(1U << ICMP_ECHOREPLY);  // 只放行回显应答
    (void)::setsockopt(n.fd, SOL_RAW, ICMP_FILTER, &filt, sizeof(filt));
    // 同一网卡的多个目标错开发送
    const auto nt = static_cast<int64_t>(s.targets.size());
    for (int64_t k = 0; k < nt; ++k) n.targets.push_back({s.targets[static_cast<std::size_t>(k)],
                                                          now + im.interval_ns * k / nt});
    im.nics.push_back(std::move(n));
  }

  im.epfd = ::epoll_create1(EPOLL_CLOEXEC);
  im.timerfd = ::timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
  im.stopfd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
  if (im.epfd < 0 || im.timerfd < 0 || im.stopfd < 0) {
    throw std::runtime_error(std::string("网卡探测初始化 epoll/timerfd 失败：") + std::strerror(errno));
  }
  auto add = [&](int fd, uint32_t id) {
    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.u32 = id;
    if (::epoll_ctl(im.epfd, EPOLL_CTL_ADD, fd, &ev) != 0) {
      throw std::runtime_error(std::string("epoll_ctl 失败：") + std::strerror(errno));
    }
  };
  for (uint32_t i = 0; i < im.nics.size(); ++i) add(im.nics[i].fd, i);
  add(im.timerfd, static_cast<uint32_t>(im.nics.size()));
  add(im.stopfd, static_cast<uint32_t>(im.nics.size() + 1));
}

Prober::~Prober() { stop(); }

void Prober::start() {
  auto& im = *impl_;
  if (im.running.exchange(true)) return;
  itimerspec its{};
  its.it_value.tv_nsec = 1;  // 立即触发第一次
  its.it_interval.tv_nsec = kTickMs * 1000000L;
  ::timerfd_settime(im.timerfd, 0, &its, nullptr);
  im.thread = std::thread([this] { impl_->loop(); });
}

void Prober::stop() {
  if (!impl_) return;
  auto& im = *impl_;
  if (!im.running.exchange(false)) return;
  const uint64_t one = 1;
  (void)!::write(im.stopfd, &one, sizeof(one));
  if (im.thread.joinable()) im.thread.join();
}

std::vector<ProbePeriod> Prober::take_period() {
  auto& im = *impl_;
  std::vector<ProbePeriod> out;
  out.reserve(im.nics.size());
  std::lock_guard<std::mutex> lk(im.mu);
  for (auto& n : im.nics) out.push_back(n.state.take());
  return out;
}

}  // namespace lm::worker

#else  // !__linux__

namespace lm::worker {

struct Prober::Impl {};

Prober::Prober(const NetProbeConfig&, const std::vector<NicSpec>&) {
  throw std::runtime_error("网卡探测只支持 Linux");
}
Prober::~Prober() = default;
void Prober::start() {}
void Prober::stop() {}
std::vector<ProbePeriod> Prober::take_period() { return {}; }

}  // namespace lm::worker

#endif
