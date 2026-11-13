#include "nic_monitor.h"

#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <time.h>

#include <algorithm>
#include <stdexcept>

#include <spdlog/spdlog.h>

#include "prober.h"
#include "sock_diag.h"

namespace lm::worker {

namespace {
constexpr int kErrorAfterFails = 10;

void note_failure(const char* what, int& fails) {
  ++fails;
  if (fails == kErrorAfterFails) {
    spdlog::error("{} 连续 {} 次读取失败，沿用上次的值", what, fails);
  } else if (fails < kErrorAfterFails) {
    spdlog::warn("{} 读取失败，沿用上次的值", what);
  }
}

void note_ok(const char* what, int& fails) {
  if (fails >= kErrorAfterFails) spdlog::info("{} 读取恢复", what);
  fails = 0;
}
}  // namespace

NicMonitor::NicMonitor(std::vector<std::string> names, Sources src)
    : names_(std::move(names)),
      src_(std::move(src)),
      retrans_(src_.ip_to_nic, static_cast<int>(names_.size())),
      rx_(names_.size()),
      tx_(names_.size()),
      last_retrans_(names_.size(), 0.0),
      last_rx_(names_.size(), 0),
      last_tx_(names_.size(), 0) {}

NicMonitor::~NicMonitor() {
  if (src_.stop) src_.stop();
}

std::vector<lm::NicStat> NicMonitor::collect() {
  const std::size_t n = names_.size();
  std::vector<ProbePeriod> probe = src_.probe ? src_.probe() : std::vector<ProbePeriod>{};
  probe.resize(n);

  if (auto conns = src_.tcp ? src_.tcp() : std::nullopt) {
    note_ok("TCP 重传统计", tcp_fails_);
    last_retrans_ = retrans_.update(*conns);
  } else {
    note_failure("TCP 重传统计", tcp_fails_);
  }

  const int64_t now = src_.now_ns ? src_.now_ns() : 0;
  bool bytes_ok = true;
  for (std::size_t i = 0; i < n; ++i) {
    auto rx = src_.bytes ? src_.bytes(i, true) : std::nullopt;
    auto tx = src_.bytes ? src_.bytes(i, false) : std::nullopt;
    if (rx && tx) {
      last_rx_[i] = rx_[i].update(*rx, now);
      last_tx_[i] = tx_[i].update(*tx, now);
    } else {
      bytes_ok = false;
    }
  }
  if (bytes_ok) {
    note_ok("网卡流量", bytes_fails_);
  } else {
    note_failure("网卡流量", bytes_fails_);
  }

  std::vector<lm::NicStat> out(n);
  for (std::size_t i = 0; i < n; ++i) {
    out[i].set_name(names_[i]);
    out[i].set_rtt_ms(static_cast<float>(probe[i].rtt_ms));
    out[i].set_rtt_jitter_ms(static_cast<float>(probe[i].jitter_ms));
    out[i].set_timeout_rate(static_cast<float>(probe[i].timeout_rate));
    out[i].set_retrans_rate(static_cast<float>(last_retrans_[i]));
    out[i].set_rx_bps(last_rx_[i]);
    out[i].set_tx_bps(last_tx_[i]);
  }
  return out;
}

std::unique_ptr<NicMonitor> NicMonitor::create(const NetProbeConfig& cfg,
                                               const std::vector<NicSpec>& nics) {
  if (nics.empty()) throw std::runtime_error("没有可探测的网卡");
  auto prober = std::make_shared<Prober>(cfg, nics);  // 失败抛出（如缺少 CAP_NET_RAW）
  auto diag = std::make_shared<SockDiag>();

  std::vector<std::string> names;
  auto counters = std::make_shared<std::vector<std::unique_ptr<SysfsCounter>>>();
  for (const auto& s : nics) {
    names.push_back(s.name);
    const std::string base = "/sys/class/net/" + s.name + "/statistics/";
    counters->push_back(std::make_unique<SysfsCounter>(base + "rx_bytes"));
    counters->push_back(std::make_unique<SysfsCounter>(base + "tx_bytes"));
  }

  Sources src;
  // 本地 IPv4 → 网卡编号（启动时确定）
  ifaddrs* ifa = nullptr;
  if (getifaddrs(&ifa) == 0) {
    for (ifaddrs* p = ifa; p != nullptr; p = p->ifa_next) {
      if (p->ifa_addr == nullptr || p->ifa_addr->sa_family != AF_INET) continue;
      for (std::size_t i = 0; i < names.size(); ++i) {
        if (names[i] == p->ifa_name) {
          src.ip_to_nic[reinterpret_cast<sockaddr_in*>(p->ifa_addr)->sin_addr.s_addr] =
              static_cast<int>(i);
        }
      }
    }
    freeifaddrs(ifa);
  }
  src.probe = [prober] { return prober->take_period(); };
  src.tcp = [diag] { return diag->dump(); };
  src.bytes = [counters](std::size_t nic, bool rx) {
    return (*counters)[nic * 2 + (rx ? 0 : 1)]->read();
  };
  src.now_ns = [] {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
  };
  src.stop = [prober] { prober->stop(); };

  auto mon = std::make_unique<NicMonitor>(std::move(names), std::move(src));
  prober->start();
  return mon;
}

}  // namespace lm::worker
