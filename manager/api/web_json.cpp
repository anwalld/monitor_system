#include "web_json.h"

#include <algorithm>
#include <cstdlib>
#include <map>

#include "storage/json_util.h"

namespace lm::manager {

namespace {

std::string opt_num(bool has, double v) { return has ? json_num(v) : "null"; }

std::pair<uint64_t, uint64_t> parse_id(const std::string& s) {
  const auto dash = s.find('-');
  char* end = nullptr;
  const uint64_t ms = std::strtoull(s.c_str(), &end, 10);
  if (end == s.c_str()) return {0, 0};
  const uint64_t seq = dash == std::string::npos ? 0 : std::strtoull(s.c_str() + dash + 1, nullptr, 10);
  return {ms, seq};
}

}  // namespace

std::string node_json(const NodeView& v) {
  const NodeState& n = v.st;
  const lm::Sample& s = n.latest;
  JsonObj parts;
  parts.add("cpu", n.score.cpu).add("runq", n.score.runq).add("mem", n.score.mem).add("net", n.score.net)
      .add_str("best_nic", n.score.best_nic);
  JsonObj infl;
  infl.add_int("inflight", v.inflight.inflight_tasks)
      .add_int("inflight_bytes", v.inflight.inflight_bytes)
      .add_int("unconfirmed", static_cast<long long>(v.inflight.unconfirmed))
      .add("f_inflight", v.inflight.f_inflight)
      .add("f_timeout", v.inflight.f_timeout)
      .add("timeout_count", v.inflight.timeout_count);
  std::string nics = "[";
  for (int i = 0; i < s.nics_size(); ++i) {
    const auto& c = s.nics(i);
    if (i > 0) nics += ',';
    nics += JsonObj()
                .add_str("name", c.name())
                .add("rtt_ms", c.rtt_ms())
                .add("timeout_rate", c.timeout_rate())
                .add("retrans_rate", c.retrans_rate())
                .add_int("rx_bps", static_cast<long long>(c.rx_bps()))
                .add_int("tx_bps", static_cast<long long>(c.tx_bps()))
                .str();
  }
  nics += "]";
  return JsonObj()
      .add_str("id", n.node_id)
      .add_str("addr", n.addr)
      .add_int("ncpu", n.ncpu)
      .raw("online", v.online ? "true" : "false")
      .add_int("age_ms", v.age_ms)
      .add_int("first_seen_ms", n.first_seen_wall_ms)
      .add_int("last_seen_ms", n.last_seen_wall_ms)
      .add_str("state", mode_name(n.br.mode))
      .add_str("state_reason", n.br.reason)
      .add_int("state_since_ms", n.br.since_wall_ms)
      .add("f_slow", v.f_slow)
      .add("score", n.score.total)
      .raw("score_parts", parts.str())
      .raw("evicted", n.evict.evicted() ? "true" : "false")
      .add_str("evict_reason", n.evict.reason())
      .add("prob", n.prob)
      .add("cpu", s.cpu_busy())
      .add("mem", s.mem_avail_ratio())
      .add("disk", s.disk_util())
      .raw("runq_p99_us", opt_num(s.has_runq_p99_us(), s.runq_p99_us()))
      .raw("inflight", infl.str())
      .raw("nics", nics)
      .str();
}

std::string nodes_array_json(const std::vector<NodeView>& vs) {
  std::string out = "[";
  for (std::size_t i = 0; i < vs.size(); ++i) {
    if (i > 0) out += ',';
    out += node_json(vs[i]);
  }
  return out + "]";
}

std::string field_of(const StreamFields& f, const std::string& k) {
  for (const auto& [a, b] : f) {
    if (a == k) return b;
  }
  return "";
}

std::string event_json(const std::string& id, const StreamFields& fields, bool with_type) {
  JsonObj o;
  if (with_type) o.add_str("type", "event");
  std::string detail = field_of(fields, "detail");
  // detail 是 Manager / AI 写入的 JSON 对象，原样嵌入；明显不是对象时当字符串
  const auto first = detail.find_first_not_of(" \t\r\n");
  const bool is_obj = first != std::string::npos && detail[first] == '{' && detail.back() == '}';
  const std::string ts = field_of(fields, "ts_ms");
  char* end = nullptr;
  const long long ts_v = std::strtoll(ts.c_str(), &end, 10);
  o.add_str("id", id)
      .add_str("node", field_of(fields, "node"))
      .add_str("kind", field_of(fields, "kind"))
      .add_int("ts", end != ts.c_str() ? ts_v : static_cast<long long>(parse_id(id).first))
      .raw("detail", detail.empty() ? "{}" : is_obj ? detail : json_str(detail));
  return o.str();
}

std::vector<SeriesPoint> downsample(const std::vector<lm::Sample>& samples, int64_t step_ms) {
  if (step_ms <= 0) step_ms = 1;
  struct Acc {
    SeriesPoint p;
    double rtt = 0, to = 0, rt = 0;
    int net_n = 0;
  };
  std::map<int64_t, Acc> buckets;
  for (const auto& s : samples) {
    const int64_t ts = s.ts_ms();
    const int64_t b = ts - ((ts % step_ms) + step_ms) % step_ms;
    Acc& a = buckets[b];
    a.p.ts_ms = b;
    ++a.p.n;
    a.p.cpu += s.cpu_busy();
    a.p.mem += s.mem_avail_ratio();
    a.p.disk += s.disk_util();
    if (s.has_runq_p99_us()) {
      const double v = s.runq_p99_us();
      a.p.runq_p99_us = a.p.runq_p99_us ? std::max(*a.p.runq_p99_us, v) : v;
    }
    if (s.nics_size() > 0) {
      double rtt = 0, to = 0, rt = 0;
      for (const auto& c : s.nics()) {
        rtt += c.rtt_ms();
        to += c.timeout_rate();
        rt += c.retrans_rate();
        a.p.rx_bps += static_cast<double>(c.rx_bps());
        a.p.tx_bps += static_cast<double>(c.tx_bps());
      }
      const double k = s.nics_size();
      a.rtt += rtt / k;
      a.to += to / k;
      a.rt += rt / k;
      ++a.net_n;
    }
  }
  std::vector<SeriesPoint> out;
  out.reserve(buckets.size());
  for (auto& [b, a] : buckets) {
    SeriesPoint p = a.p;
    const double n = p.n;
    p.cpu /= n;
    p.mem /= n;
    p.disk /= n;
    p.rx_bps /= n;
    p.tx_bps /= n;
    if (a.net_n > 0) {
      p.rtt_ms = a.rtt / a.net_n;
      p.timeout_rate = a.to / a.net_n;
      p.retrans_rate = a.rt / a.net_n;
    }
    out.push_back(p);
  }
  return out;
}

std::string series_json(const std::string& node_id, int64_t step_ms,
                        const std::vector<SeriesPoint>& pts) {
  auto on = [](const std::optional<double>& v) { return v ? json_num(*v) : std::string("null"); };
  std::string arr = "[";
  for (std::size_t i = 0; i < pts.size(); ++i) {
    const auto& p = pts[i];
    if (i > 0) arr += ',';
    arr += JsonObj()
               .add_int("ts", p.ts_ms)
               .add_int("n", p.n)
               .add("cpu", p.cpu)
               .add("mem", p.mem)
               .add("disk", p.disk)
               .raw("runq_p99_us", on(p.runq_p99_us))
               .raw("rtt_ms", on(p.rtt_ms))
               .raw("timeout_rate", on(p.timeout_rate))
               .raw("retrans_rate", on(p.retrans_rate))
               .add("rx_bps", p.rx_bps)
               .add("tx_bps", p.tx_bps)
               .str();
  }
  arr += "]";
  return JsonObj().add_str("node", node_id).add_int("step_ms", step_ms).raw("points", arr).str();
}

bool token_ok(const std::string& expected, const std::string& got) {
  if (expected.empty()) return true;
  unsigned diff = expected.size() != got.size() ? 1U : 0U;
  for (std::size_t i = 0; i < expected.size(); ++i) {
    const char g = i < got.size() ? got[i] : '\0';
    diff |= static_cast<unsigned>(static_cast<unsigned char>(expected[i]) ^ static_cast<unsigned char>(g));
  }
  return diff == 0;
}

std::string bearer_token(const std::string& authorization) {
  const std::string p = "Bearer ";
  if (authorization.size() <= p.size() || authorization.compare(0, p.size(), p) != 0) return "";
  return authorization.substr(p.size());
}

bool stream_id_less(const std::string& a, const std::string& b) { return parse_id(a) < parse_id(b); }

}  // namespace lm::manager
