#include "route.h"

#if defined(__linux__)

#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstring>
#include <vector>

namespace lm::worker {

std::map<unsigned, uint32_t> default_gateways() {
  struct Best {
    uint32_t gw;
    uint32_t table;
  };
  std::map<unsigned, Best> best;

  const int fd = ::socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
  if (fd < 0) return {};
  struct {
    nlmsghdr nh;
    rtmsg rt;
  } req{};
  req.nh.nlmsg_len = NLMSG_LENGTH(sizeof(rtmsg));
  req.nh.nlmsg_type = RTM_GETROUTE;
  req.nh.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
  req.nh.nlmsg_seq = 1;
  req.rt.rtm_family = AF_INET;
  req.rt.rtm_table = RT_TABLE_UNSPEC;  // 所有表
  if (::send(fd, &req, req.nh.nlmsg_len, 0) < 0) {
    ::close(fd);
    return {};
  }

  std::vector<char> buf(32768);
  bool done = false;
  while (!done) {
    const ssize_t n = ::recv(fd, buf.data(), buf.size(), 0);
    if (n <= 0) break;
    int len = static_cast<int>(n);
    for (auto* nh = reinterpret_cast<nlmsghdr*>(buf.data()); NLMSG_OK(nh, len);
         nh = NLMSG_NEXT(nh, len)) {
      if (nh->nlmsg_type == NLMSG_DONE || nh->nlmsg_type == NLMSG_ERROR) {
        done = true;
        break;
      }
      if (nh->nlmsg_type != RTM_NEWROUTE) continue;
      auto* rt = static_cast<rtmsg*>(NLMSG_DATA(nh));
      if (rt->rtm_family != AF_INET || rt->rtm_dst_len != 0 || rt->rtm_type != RTN_UNICAST) continue;
      uint32_t table = rt->rtm_table;
      uint32_t gw = 0;
      unsigned oif = 0;
      int alen = static_cast<int>(RTM_PAYLOAD(nh));
      for (auto* a = RTM_RTA(rt); RTA_OK(a, alen); a = RTA_NEXT(a, alen)) {
        if (a->rta_type == RTA_TABLE) std::memcpy(&table, RTA_DATA(a), sizeof(table));
        if (a->rta_type == RTA_GATEWAY) std::memcpy(&gw, RTA_DATA(a), sizeof(gw));
        if (a->rta_type == RTA_OIF) std::memcpy(&oif, RTA_DATA(a), sizeof(oif));
      }
      if (gw == 0 || oif == 0) continue;
      // 主表优先，其次表号小的
      auto rank = [](uint32_t t) { return t == RT_TABLE_MAIN ? 0U : t + 1; };
      auto it = best.find(oif);
      if (it == best.end() || rank(table) < rank(it->second.table)) best[oif] = {gw, table};
    }
  }
  ::close(fd);

  std::map<unsigned, uint32_t> out;
  for (const auto& [oif, b] : best) out[oif] = b.gw;
  return out;
}

}  // namespace lm::worker

#else

namespace lm::worker {
std::map<unsigned, uint32_t> default_gateways() { return {}; }
}  // namespace lm::worker

#endif
