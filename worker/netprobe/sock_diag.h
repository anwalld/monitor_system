#pragma once

// 用 Netlink NETLINK_SOCK_DIAG 取所有 IPv4 ESTABLISHED TCP 连接的 tcp_info（仅 Linux）。

#include <optional>
#include <vector>

#include "retrans.h"

namespace lm::worker {

class SockDiag {
 public:
  SockDiag();  // 打开 netlink socket；失败时 ok() 为 false
  ~SockDiag();
  SockDiag(const SockDiag&) = delete;
  SockDiag& operator=(const SockDiag&) = delete;

  bool ok() const { return fd_ >= 0; }
  // 失败返回 nullopt。
  std::optional<std::vector<TcpConnSample>> dump();

 private:
  int fd_ = -1;
  unsigned seq_ = 0;
};

}  // namespace lm::worker
