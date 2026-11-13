#include "icmp.h"

#include <arpa/inet.h>

#include <cstring>

namespace lm::worker {

namespace {
constexpr uint8_t kEchoReply = 0;
constexpr uint8_t kEchoRequest = 8;

void put_be16(uint8_t* p, uint16_t v) {
  p[0] = static_cast<uint8_t>(v >> 8);
  p[1] = static_cast<uint8_t>(v);
}
uint16_t get_be16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
void put_be32(uint8_t* p, uint32_t v) {
  for (int i = 0; i < 4; ++i) p[i] = static_cast<uint8_t>(v >> (24 - 8 * i));
}
uint32_t get_be32(const uint8_t* p) {
  return (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | p[3];
}
}  // namespace

uint16_t inet_checksum(const uint8_t* data, std::size_t len) {
  uint32_t sum = 0;
  for (std::size_t i = 0; i + 1 < len; i += 2) sum += (uint32_t{data[i]} << 8) | data[i + 1];
  if (len & 1U) sum += uint32_t{data[len - 1]} << 8;
  while (sum >> 16) sum = (sum & 0xffffU) + (sum >> 16);
  return htons(static_cast<uint16_t>(~sum & 0xffffU));
}

std::vector<uint8_t> build_echo_request(uint16_t ident, uint16_t seq, uint32_t nic_index,
                                        uint64_t sent_ns) {
  std::vector<uint8_t> p(kIcmpEchoLen, 0);
  p[0] = kEchoRequest;
  p[1] = 0;
  put_be16(&p[4], ident);
  put_be16(&p[6], seq);
  put_be32(&p[8], nic_index);
  put_be32(&p[16], static_cast<uint32_t>(sent_ns >> 32));
  put_be32(&p[20], static_cast<uint32_t>(sent_ns));
  const uint16_t c = inet_checksum(p.data(), p.size());
  std::memcpy(&p[2], &c, 2);
  return p;
}

bool parse_echo_reply(const uint8_t* pkt, std::size_t len, EchoReply& out) {
  if (len < 20) return false;
  if ((pkt[0] >> 4) != 4) return false;
  const std::size_t ihl = static_cast<std::size_t>(pkt[0] & 0x0f) * 4;
  if (ihl < 20 || len < ihl + kIcmpEchoLen) return false;
  if (pkt[9] != 1) return false;  // 协议不是 ICMP
  const uint8_t* icmp = pkt + ihl;
  const std::size_t icmp_len = len - ihl;
  if (icmp[0] != kEchoReply || icmp[1] != 0) return false;
  if (inet_checksum(icmp, icmp_len) != 0) return false;
  std::memcpy(&out.src, pkt + 12, 4);
  out.ident = get_be16(icmp + 4);
  out.seq = get_be16(icmp + 6);
  out.nic_index = get_be32(icmp + 8);
  return true;
}

}  // namespace lm::worker
