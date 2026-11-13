#include "retrans.h"

#include <algorithm>

namespace lm::worker {

std::vector<double> RetransTracker::update(const std::vector<TcpConnSample>& conns) {
  std::vector<uint64_t> d_retrans(static_cast<std::size_t>(nnic_), 0);
  std::vector<uint64_t> d_segs(static_cast<std::size_t>(nnic_), 0);
  std::unordered_map<uint64_t, Prev> next;
  next.reserve(conns.size());

  for (const auto& c : conns) {
    auto nic = ip_to_nic_.find(c.local_ip);
    if (nic == ip_to_nic_.end()) continue;  // 回环或不参与探测的网卡
    auto it = prev_.find(c.cookie);
    if (it != prev_.end()) {
      // 32 位累计计数，按模运算求差（同一 cookie 只增不减，变小就是回绕）
      const auto i = static_cast<std::size_t>(nic->second);
      d_retrans[i] += static_cast<uint32_t>(c.total_retrans - it->second.retrans);
      d_segs[i] += static_cast<uint32_t>(c.segs_out - it->second.segs);
    }
    next[c.cookie] = {c.total_retrans, c.segs_out};  // 新连接只记基线；消失的连接自然被丢弃
  }
  prev_.swap(next);

  std::vector<double> rate(static_cast<std::size_t>(nnic_), 0.0);
  for (std::size_t i = 0; i < rate.size(); ++i) {
    if (d_segs[i] > 0) {
      rate[i] = std::clamp(static_cast<double>(d_retrans[i]) / static_cast<double>(d_segs[i]), 0.0, 1.0);
    }
  }
  return rate;
}

}  // namespace lm::worker
