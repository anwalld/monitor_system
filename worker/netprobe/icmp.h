#pragma once

// ICMP 回显请求构造与回显应答解析（纯逻辑，与平台无关）。
// 请求 = 8 字节 ICMP 头 + 16 字节数据：网卡编号 u32、保留 u32、发送时间 u64（单调时钟 ns）。
// 原始 IPv4 ICMP socket 收到的数据带 IPv4 头，parse_echo_reply 按 IHL 跳过。

#include <cstddef>
#include <cstdint>
#include <vector>

namespace lm::worker {

inline constexpr std::size_t kIcmpHeader = 8;
inline constexpr std::size_t kIcmpPayload = 16;
inline constexpr std::size_t kIcmpEchoLen = kIcmpHeader + kIcmpPayload;

// RFC 1071 互联网校验和（结果为网络字节序，可直接写入报文）。
uint16_t inet_checksum(const uint8_t* data, std::size_t len);

std::vector<uint8_t> build_echo_request(uint16_t ident, uint16_t seq, uint32_t nic_index,
                                        uint64_t sent_ns);

struct EchoReply {
  uint32_t src = 0;  // 源地址，网络字节序
  uint16_t ident = 0;
  uint16_t seq = 0;
  uint32_t nic_index = 0;
};

// 解析带 IPv4 头的报文；不是合法的回显应答（类型不对、长度不够、校验和错误）返回 false。
bool parse_echo_reply(const uint8_t* pkt, std::size_t len, EchoReply& out);

}  // namespace lm::worker
