#include "node_table.h"

#include <algorithm>

namespace lm::manager {

std::optional<std::string> NodeTable::validate(const lm::Sample& s) const {
  if (s.node_id().empty()) return std::string("node_id 不能为空");
  if (s.node_id().size() > opt_.max_node_id_len) {
    return "node_id 超过 " + std::to_string(opt_.max_node_id_len) + " 字节";
  }
  return std::nullopt;
}

NodeTable::Result NodeTable::apply(const lm::Sample& s, std::string_view peer_ip,
                                   int64_t now_mono_ms, int64_t now_wall_ms,
                                   AcceptedSample* out) {
  if (validate(s)) return Result::kInvalid;

  std::lock_guard<std::mutex> lk(mu_);
  auto [it, inserted] = nodes_.try_emplace(s.node_id());
  NodeState& n = it->second;
  if (inserted) {
    n.node_id = s.node_id();
    n.first_seen_wall_ms = now_wall_ms;
  } else if (s.seq() <= n.last_seq) {
    // 乱序或重复；只有在节点已经沉默超过 restart_gap 时才认为是 Worker 重启后的新进程
    if (now_mono_ms - n.last_seen_mono_ms <= opt_.restart_gap_ms) return Result::kStale;
  }

  n.addr = join_host_port(peer_ip, opt_.worker_port);
  n.ncpu = static_cast<uint32_t>(s.core_busy_size());
  n.last_seq = s.seq();
  n.last_seen_mono_ms = now_mono_ms;
  n.last_seen_wall_ms = now_wall_ms;
  n.latest = s;

  const bool was_evicted = n.evict.evicted();
  n.score = compute_score(s, opt_.scoring);
  n.evict.update(s, opt_.scoring);
  const bool hint_runq = n.score.runq_missing && !n.hinted_runq_missing;
  const bool hint_net = n.score.net_missing && !n.hinted_net_missing;
  n.hinted_runq_missing = n.score.runq_missing;  // 恢复上报后清零，再次缺失会再提示
  n.hinted_net_missing = n.score.net_missing;

  if (out != nullptr) {
    out->addr = n.addr;
    out->ncpu = n.ncpu;
    out->recv_wall_ms = now_wall_ms;
    out->new_node = inserted;
    out->sample = s;
    out->score = n.score;
    out->evicted = n.evict.evicted();
    out->evict_reason = n.evict.reason();
    out->evicted_changed = out->evicted != was_evicted;
    out->hint_runq_missing = hint_runq;
    out->hint_net_missing = hint_net;
    out->inflight = ledger_.snapshot(n.node_id);
  }
  return Result::kAccepted;
}

NodeTable::Counts NodeTable::counts(int64_t now_mono_ms) const {
  std::lock_guard<std::mutex> lk(mu_);
  Counts c;
  c.total = nodes_.size();
  for (const auto& [id, n] : nodes_) {
    if (now_mono_ms - n.last_seen_mono_ms <= opt_.online_window_ms) ++c.online;
  }
  return c;
}

std::optional<NodeState> NodeTable::get(const std::string& node_id) const {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = nodes_.find(node_id);
  if (it == nodes_.end()) return std::nullopt;
  return it->second;
}

std::optional<int64_t> NodeTable::age_ms(const std::string& node_id, int64_t now_mono_ms) const {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = nodes_.find(node_id);
  if (it == nodes_.end()) return std::nullopt;
  return now_mono_ms - it->second.last_seen_mono_ms;
}

std::vector<std::string> NodeTable::node_ids() const {
  std::lock_guard<std::mutex> lk(mu_);
  std::vector<std::string> ids;
  ids.reserve(nodes_.size());
  for (const auto& [id, n] : nodes_) ids.push_back(id);
  return ids;
}

SelectResult NodeTable::select(int64_t now_mono_ms, double u) const {
  std::lock_guard<std::mutex> lk(mu_);
  return select_locked(now_mono_ms, u);
}

SelectResult NodeTable::allocate(int64_t now_mono_ms, double u, const std::string& task_id,
                                 uint64_t input_bytes) {
  std::lock_guard<std::mutex> lk(mu_);
  SelectResult r = select_locked(now_mono_ms, u);
  if (r.pick) ledger_.add(task_id, r.pick->node_id, input_bytes, now_mono_ms);
  return r;
}

InflightLedger::MarkResult NodeTable::mark_sent(const std::string& task_id, int64_t now_mono_ms) {
  std::lock_guard<std::mutex> lk(mu_);
  return ledger_.mark_sent(task_id, now_mono_ms);
}

InflightLedger::EventResult NodeTable::on_event(const std::string& task_id,
                                                lm::TaskEvent::Kind kind, int64_t now_mono_ms) {
  std::lock_guard<std::mutex> lk(mu_);
  return ledger_.on_event(task_id, kind, now_mono_ms);
}

void NodeTable::tick(int64_t now_mono_ms, std::vector<InflightLedger::Expired>* expired) {
  std::lock_guard<std::mutex> lk(mu_);
  ledger_.tick(now_mono_ms, expired);
}

void NodeTable::tick_second(double dt_s) {
  std::lock_guard<std::mutex> lk(mu_);
  ledger_.tick_second(dt_s);
}

InflightLedger::Totals NodeTable::inflight_totals() const {
  std::lock_guard<std::mutex> lk(mu_);
  return ledger_.totals();
}

InflightSnapshot NodeTable::inflight_snapshot(const std::string& node_id) const {
  std::lock_guard<std::mutex> lk(mu_);
  return ledger_.snapshot(node_id);
}

SelectResult NodeTable::select_locked(int64_t now_mono_ms, double u) const {
  struct Cand {
    const NodeState* n;
    double w;
  };
  std::vector<Cand> cands;
  SelectResult r;
  r.total = nodes_.size();
  cands.reserve(nodes_.size());
  for (const auto& [id, n] : nodes_) {
    if (now_mono_ms - n.last_seen_mono_ms > opt_.scoring.lost_after_ms) {
      ++r.lost;
      continue;
    }
    if (n.evict.evicted()) {
      ++r.evicted;
      continue;
    }
    const double w = weight(n.score.total, ledger_.factors(id));
    if (w > 0) cands.push_back({&n, w});
  }
  r.candidates = cands.size();
  if (cands.empty()) return r;
  std::sort(cands.begin(), cands.end(),
            [](const Cand& a, const Cand& b) { return a.n->node_id < b.n->node_id; });
  std::vector<double> prefix;
  prefix.reserve(cands.size());
  double acc = 0;
  for (const auto& c : cands) prefix.push_back(acc += c.w);
  const auto i = pick_weighted(prefix, u);
  if (!i) return r;
  const Cand& c = cands[*i];
  r.pick = SelectResult::Pick{c.n->node_id, c.n->addr, c.w, c.w / acc};
  return r;
}

std::string parse_peer_ip(std::string_view peer) {
  std::string p(peer);
  // 解码 gRPC 对 IPv6 方括号的百分号编码
  for (auto [from, to] : {std::pair{"%5B", "["}, std::pair{"%5D", "]"}}) {
    for (std::size_t pos; (pos = p.find(from)) != std::string::npos;) p.replace(pos, 3, to);
  }
  std::string_view v(p);
  if (v.substr(0, 5) == "ipv4:") {
    v.remove_prefix(5);
    const auto colon = v.rfind(':');
    return std::string(colon == std::string_view::npos ? v : v.substr(0, colon));
  }
  if (v.substr(0, 5) == "ipv6:") {
    v.remove_prefix(5);
    if (!v.empty() && v.front() == '[') {
      const auto close = v.find(']');
      if (close != std::string_view::npos) return std::string(v.substr(1, close - 1));
    }
    return std::string(v);
  }
  return std::string(v);  // unix: 等其他形式原样返回
}

std::string join_host_port(std::string_view ip, uint16_t port) {
  if (ip.find(':') != std::string_view::npos) {
    return "[" + std::string(ip) + "]:" + std::to_string(port);
  }
  return std::string(ip) + ":" + std::to_string(port);
}

}  // namespace lm::manager
