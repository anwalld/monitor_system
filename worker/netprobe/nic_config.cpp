#include "nic_config.h"

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <unistd.h>

#include <map>

#include "route.h"

namespace lm::worker {

bool parse_ipv4(const std::string& s, uint32_t& out) {
  in_addr a{};
  if (inet_pton(AF_INET, s.c_str(), &a) != 1) return false;
  out = a.s_addr;
  return true;
}

std::string ipv4_str(uint32_t addr) {
  char buf[INET_ADDRSTRLEN] = {};
  in_addr a{};
  a.s_addr = addr;
  inet_ntop(AF_INET, &a, buf, sizeof(buf));
  return buf;
}

namespace {

bool sys_nic_exists(const std::string& name) {
  return !name.empty() && name.find('/') == std::string::npos &&
         ::access(("/sys/class/net/" + name).c_str(), F_OK) == 0;
}

}  // namespace

NetProbeConfig parse_net_config(const lm::Config& cfg,
                                const std::function<bool(const std::string&)>& nic_exists) {
  const auto exists = nic_exists ? nic_exists : sys_nic_exists;
  NetProbeConfig out;
  out.interval_ms = cfg.get<int>("net.probe_interval_ms", 200);
  out.timeout_ms = cfg.get<int>("net.probe_timeout_ms", 1000);
  if (out.interval_ms < 50) {
    throw lm::ConfigError("net.probe_interval_ms 不能小于 50，当前 " + std::to_string(out.interval_ms));
  }
  if (out.timeout_ms < out.interval_ms) {
    throw lm::ConfigError("net.probe_timeout_ms 不能小于 net.probe_interval_ms");
  }

  const YAML::Node nics = cfg.node("net.nics");
  if (!nics.IsDefined() || nics.IsNull()) return out;
  if (!nics.IsSequence()) throw lm::ConfigError("net.nics 必须是列表");
  for (std::size_t i = 0; i < nics.size(); ++i) {
    const YAML::Node n = nics[i];
    NicSpec spec;
    try {
      spec.name = n["name"].as<std::string>();
    } catch (const YAML::Exception&) {
      throw lm::ConfigError("net.nics[" + std::to_string(i) + "] 缺少 name");
    }
    if (!exists(spec.name)) throw lm::ConfigError("net.nics 中的网卡不存在：" + spec.name);
    const YAML::Node t = n["targets"];
    if (!t.IsSequence() || t.size() == 0) {
      throw lm::ConfigError("网卡 " + spec.name + " 的 targets 必须是 1 到 3 个 IPv4 地址");
    }
    if (t.size() > static_cast<std::size_t>(kMaxTargetsPerNic)) {
      throw lm::ConfigError("网卡 " + spec.name + " 的目标超过 3 个");
    }
    for (std::size_t k = 0; k < t.size(); ++k) {
      const std::string s = t[k].as<std::string>("");
      uint32_t addr = 0;
      if (!parse_ipv4(s, addr)) {
        throw lm::ConfigError("网卡 " + spec.name + " 的目标不是合法 IPv4 地址：" + s);
      }
      spec.targets.push_back(addr);
      spec.target_strs.push_back(s);
    }
    for (const auto& prev : out.nics) {
      if (prev.name == spec.name) throw lm::ConfigError("net.nics 中网卡重复：" + spec.name);
    }
    out.nics.push_back(std::move(spec));
  }
  return out;
}

std::vector<NicSpec> discover_nics(std::vector<std::string>* skipped) {
  std::vector<NicSpec> out;
#if defined(__linux__)
  ifaddrs* ifa = nullptr;
  if (getifaddrs(&ifa) != 0) return out;
  std::vector<std::pair<std::string, unsigned>> cands;  // 名字、ifindex，按 getifaddrs 顺序去重
  for (ifaddrs* p = ifa; p != nullptr; p = p->ifa_next) {
    if (p->ifa_addr == nullptr || p->ifa_addr->sa_family != AF_INET) continue;
    if (!(p->ifa_flags & IFF_UP) || (p->ifa_flags & IFF_LOOPBACK)) continue;
    const std::string name = p->ifa_name;
    if (::access(("/sys/class/net/" + name + "/device").c_str(), F_OK) != 0) continue;
    bool dup = false;
    for (const auto& c : cands) dup = dup || c.first == name;
    if (!dup) cands.emplace_back(name, if_nametoindex(name.c_str()));
  }
  freeifaddrs(ifa);

  const std::map<unsigned, uint32_t> gws = default_gateways();
  for (const auto& [name, idx] : cands) {
    auto it = gws.find(idx);
    if (it == gws.end()) {
      if (skipped) skipped->push_back(name + "（没有默认网关）");
      continue;
    }
    NicSpec spec;
    spec.name = name;
    spec.targets.push_back(it->second);
    spec.target_strs.push_back(ipv4_str(it->second));
    out.push_back(std::move(spec));
  }
#else
  (void)skipped;
#endif
  return out;
}

}  // namespace lm::worker
