#include "sock_diag.h"

#if defined(__linux__)

#include <linux/inet_diag.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>  // rtattr、RTA_*
#include <linux/sock_diag.h>
#include <linux/tcp.h>  // struct tcp_info（含 tcpi_segs_out）；不与 <netinet/tcp.h> 同时包含
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstddef>
#include <cstring>

namespace lm::worker {

namespace {
constexpr int kTcpEstablished = 1;  // TCP_ESTABLISHED
}

SockDiag::SockDiag() { fd_ = ::socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC, NETLINK_SOCK_DIAG); }

SockDiag::~SockDiag() {
  if (fd_ >= 0) ::close(fd_);
}

std::optional<std::vector<TcpConnSample>> SockDiag::dump() {
  if (fd_ < 0) return std::nullopt;
  struct {
    nlmsghdr nh;
    inet_diag_req_v2 r;
  } req{};
  req.nh.nlmsg_len = sizeof(req);
  req.nh.nlmsg_type = SOCK_DIAG_BY_FAMILY;
  req.nh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
  req.nh.nlmsg_seq = ++seq_;
  req.r.sdiag_family = AF_INET;
  req.r.sdiag_protocol = IPPROTO_TCP;
  req.r.idiag_states = 1U << kTcpEstablished;
  req.r.idiag_ext = 1U << (INET_DIAG_INFO - 1);
  sockaddr_nl sa{};
  sa.nl_family = AF_NETLINK;
  if (::sendto(fd_, &req, sizeof(req), 0, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) < 0) {
    return std::nullopt;
  }

  constexpr std::size_t kNeed = offsetof(tcp_info, tcpi_segs_out) + sizeof(uint32_t);
  std::vector<TcpConnSample> out;
  alignas(nlmsghdr) char buf[32768];
  for (;;) {
    const ssize_t n = ::recv(fd_, buf, sizeof(buf), 0);
    if (n < 0) return std::nullopt;
    if (n == 0) break;
    int len = static_cast<int>(n);
    for (auto* nh = reinterpret_cast<nlmsghdr*>(buf); NLMSG_OK(nh, len); nh = NLMSG_NEXT(nh, len)) {
      if (nh->nlmsg_seq != seq_) continue;  // 之前超时请求的残留应答
      if (nh->nlmsg_type == NLMSG_DONE) return out;
      if (nh->nlmsg_type == NLMSG_ERROR) return std::nullopt;
      if (nh->nlmsg_type != SOCK_DIAG_BY_FAMILY) continue;
      auto* msg = static_cast<inet_diag_msg*>(NLMSG_DATA(nh));
      int alen = static_cast<int>(nh->nlmsg_len - NLMSG_LENGTH(sizeof(*msg)));
      for (auto* a = reinterpret_cast<rtattr*>(msg + 1); RTA_OK(a, alen); a = RTA_NEXT(a, alen)) {
        if (a->rta_type != INET_DIAG_INFO || RTA_PAYLOAD(a) < kNeed) continue;
        tcp_info ti{};
        std::memcpy(&ti, RTA_DATA(a), std::min<std::size_t>(RTA_PAYLOAD(a), sizeof(ti)));
        TcpConnSample c;
        c.cookie = (uint64_t{msg->id.idiag_cookie[1]} << 32) | msg->id.idiag_cookie[0];
        c.local_ip = msg->id.idiag_src[0];
        c.total_retrans = ti.tcpi_total_retrans;
        c.segs_out = ti.tcpi_segs_out;
        out.push_back(c);
      }
    }
  }
  return out;
}

}  // namespace lm::worker

#else

namespace lm::worker {
SockDiag::SockDiag() = default;
SockDiag::~SockDiag() = default;
std::optional<std::vector<TcpConnSample>> SockDiag::dump() { return std::nullopt; }
}  // namespace lm::worker

#endif
