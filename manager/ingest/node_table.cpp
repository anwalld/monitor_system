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
    n.br.mode = NodeMode::kSlowStart;  // 新节点 → 慢启动
    n.br.since_ms = now_mono_ms;
    n.br.since_wall_ms = now_wall_ms;
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

  if (n.br.mode == NodeMode::kEvicted) {
    n.br.good_reports = n.evict.evicted() ? 0 : n.br.good_reports + 1;
  }
  std::vector<StateChange> changes;
  evaluate(n, now_mono_ms, now_wall_ms, &changes);

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
    out->mode = n.br.mode;
    out->mode_reason = n.br.reason;
    out->mode_since_wall_ms = n.br.since_wall_ms;
    out->f_slow = slow_factor(n.br, now_mono_ms, opt_.breaker);
    out->probe_fail_streak = n.br.probe_fail_streak;
    out->node_id = n.node_id;
    if (inserted) {
      StateChange c;
      c.node_id = n.node_id;
      c.from = c.to = NodeMode::kSlowStart;
      c.since_wall_ms = now_wall_ms;
      c.f_slow = out->f_slow;
      out->change = c;
    }
    if (!changes.empty()) out->change = changes.back();
  }
  return Result::kAccepted;
}

NodeTable::Counts NodeTable::counts(int64_t now_mono_ms) const {
  std::lock_guard<std::mutex> lk(mu_);
  Counts c;
  c.total = nodes_.size();
  for (const auto& [id, n] : nodes_) {
    if (now_mono_ms - n.last_seen_mono_ms <= opt_.online_window_ms) ++c.online;
    switch (n.br.mode) {
      case NodeMode::kSlowStart: ++c.slow_start; break;
      case NodeMode::kNormal: ++c.normal; break;
      case NodeMode::kEvicted: ++c.evicted; break;
      case NodeMode::kRescue: ++c.rescue; break;
    }
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
    if (!fresh(n, now_mono_ms)) {
      ++r.lost;
      continue;
    }
    if (n.br.mode == NodeMode::kRescue) {
      ++r.rescue;
      continue;
    }
    if (n.br.mode == NodeMode::kEvicted || n.evict.evicted()) {
      ++r.evicted;
      continue;
    }
    const double w = weight(n.score.total, factors_for(n, now_mono_ms, false));
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

// ---------------- 节点状态机 ----------------

bool NodeTable::eligible(const NodeState& n, int64_t now_mono_ms) const {
  return (n.br.mode == NodeMode::kNormal || n.br.mode == NodeMode::kSlowStart) &&
         fresh(n, now_mono_ms) && !n.evict.evicted() && n.score.total > 0;
}

Factors NodeTable::factors_for(const NodeState& n, int64_t now_mono_ms, bool as_normal) const {
  Factors f = ledger_.factors(n.node_id);
  if (!as_normal && n.br.mode == NodeMode::kSlowStart) {
    f.slow = slow_factor(n.br, now_mono_ms, opt_.breaker);
    f.inflight = 1;  // 慢启动期间在途系列的二次调整暂停，只积累基线
  }
  return f;
}

std::string NodeTable::evict_reason(const NodeState& n, int64_t now_mono_ms) const {
  if (!fresh(n, now_mono_ms)) return "lost";
  if (n.evict.evicted()) return n.evict.reason();
  if (n.br.probe_fail_streak >= opt_.breaker.probe_fail_evict) return "probe";
  if (n.br.gray) return "unconfirmed";
  return "";
}

void NodeTable::set_mode(NodeState& n, NodeMode m, std::string reason, int64_t now_mono_ms,
                         int64_t now_wall_ms, std::vector<StateChange>* changes) {
  StateChange c;
  c.node_id = n.node_id;
  c.from = n.br.mode;
  c.to = m;
  c.reason = reason;
  c.since_wall_ms = now_wall_ms;
  n.br.mode = m;
  n.br.since_ms = now_mono_ms;
  n.br.since_wall_ms = now_wall_ms;
  n.br.reason = std::move(reason);
  if (m == NodeMode::kEvicted) n.br.good_reports = 0;
  if (m == NodeMode::kSlowStart && c.from == NodeMode::kEvicted) {
    ledger_.reset_node(n.node_id);  // 离开候选又回来：按新节点对待
  }
  c.f_slow = slow_factor(n.br, now_mono_ms, opt_.breaker);
  c.probe_fail_streak = n.br.probe_fail_streak;
  if (changes != nullptr) changes->push_back(std::move(c));
}

void NodeTable::evaluate(NodeState& n, int64_t now_mono_ms, int64_t now_wall_ms,
                         std::vector<StateChange>* changes) {
  switch (n.br.mode) {
    case NodeMode::kSlowStart:
    case NodeMode::kNormal: {
      std::string why = evict_reason(n, now_mono_ms);
      if (!why.empty()) {
        set_mode(n, NodeMode::kEvicted, std::move(why), now_mono_ms, now_wall_ms, changes);
      } else if (n.br.mode == NodeMode::kSlowStart &&
                 now_mono_ms - n.br.since_ms >= opt_.breaker.slow_start_ms) {
        set_mode(n, NodeMode::kNormal, "", now_mono_ms, now_wall_ms, changes);
      }
      break;
    }
    case NodeMode::kEvicted:
      if (fresh(n, now_mono_ms) && !n.evict.evicted() &&
          n.br.good_reports >= opt_.breaker.recover_reports && n.br.probe_fail_streak == 0 &&
          !n.br.gray) {
        set_mode(n, NodeMode::kSlowStart, "", now_mono_ms, now_wall_ms, changes);
      }
      break;
    case NodeMode::kRescue:
      break;  // 急救期间不剔除，结束时再判断
  }
}

void NodeTable::tick_states(int64_t now_mono_ms, int64_t now_wall_ms,
                            std::vector<StateChange>* changes, std::vector<RescueStart>* rescues) {
  std::lock_guard<std::mutex> lk(mu_);
  const BreakerParams& bp = opt_.breaker;
  // 灰色故障：最近窗口内的未确认比例与全体合并比例比较
  ledger_.trim_acks(now_mono_ms, bp.gray_window_ms);
  long all_samples = 0;
  long all_bad = 0;
  for (const auto& [id, n] : nodes_) {
    const auto w = ledger_.ack_window(id);
    all_samples += w.samples;
    all_bad += w.unconfirmed;
  }
  const double mean = all_samples > 0 ? static_cast<double>(all_bad) / all_samples : 0;
  for (auto& [id, n] : nodes_) {
    const auto w = ledger_.ack_window(id);
    const double r = w.samples > 0 ? static_cast<double>(w.unconfirmed) / w.samples : 0;
    n.br.gray = w.samples >= bp.gray_min_samples && r > bp.gray_factor * mean &&
                r >= bp.gray_min_ratio;
  }

  for (auto& [id, n] : nodes_) evaluate(n, now_mono_ms, now_wall_ms, changes);

  // 急救触发：需要全体候选的权重
  double total = 0;
  int cand = 0;
  for (const auto& [id, n] : nodes_) {
    if (!eligible(n, now_mono_ms)) continue;
    total += weight(n.score.total, factors_for(n, now_mono_ms, false));
    ++cand;
  }
  if (cand < 1 || !(total > 0)) return;
  const RescueParams& rp = bp.rescue;
  for (auto& [id, n] : nodes_) {
    if (n.br.mode != NodeMode::kNormal || !eligible(n, now_mono_ms)) continue;
    if (n.score.total < rp.min_score || !n.br.backoff.allowed(now_mono_ms)) continue;
    const Factors f = factors_for(n, now_mono_ms, false);
    if (!(f.timeout < 1) || f.timeout > f.inflight || f.timeout > f.slow || f.timeout > f.pred) {
      continue;
    }
    const double p = weight(n.score.total, f) / total;
    if (!(p < rp.prob_factor / cand)) continue;
    n.br.backoff.trigger(now_mono_ms, rp);
    set_mode(n, NodeMode::kRescue, "", now_mono_ms, now_wall_ms, changes);
    if (rescues != nullptr) {
      const auto* c = ledger_.node(id);
      rescues->push_back({id, n.addr, cand, p, c != nullptr ? c->timeout_count : 0});
    }
  }
}

std::optional<StateChange> NodeTable::on_probe(const std::string& node_id, bool ok,
                                               int64_t now_mono_ms, int64_t now_wall_ms) {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = nodes_.find(node_id);
  if (it == nodes_.end()) return std::nullopt;
  NodeState& n = it->second;
  n.br.probe_fail_streak = ok ? 0 : n.br.probe_fail_streak + 1;
  std::vector<StateChange> changes;
  evaluate(n, now_mono_ms, now_wall_ms, &changes);
  if (changes.empty()) return std::nullopt;
  return changes.back();
}

std::vector<std::pair<std::string, std::string>> NodeTable::probe_targets(
    int64_t now_mono_ms) const {
  std::lock_guard<std::mutex> lk(mu_);
  std::vector<std::pair<std::string, std::string>> out;
  for (const auto& [id, n] : nodes_) {
    if (fresh(n, now_mono_ms) && n.br.mode != NodeMode::kRescue) out.emplace_back(id, n.addr);
  }
  return out;
}

RescueProbe NodeTable::rescue_batch(const std::string& node_id, bool ok, int64_t now_mono_ms) {
  std::lock_guard<std::mutex> lk(mu_);
  RescueProbe r;
  auto it = nodes_.find(node_id);
  if (it == nodes_.end()) return r;
  r.found = true;
  if (ok) ledger_.scale_timeout(node_id, opt_.breaker.rescue.t_decay);
  const NodeState& me = it->second;
  const double mine = weight(me.score.total, factors_for(me, now_mono_ms, true));
  double total = mine;
  int cand = 1;
  for (const auto& [id, n] : nodes_) {
    if (id == node_id || !eligible(n, now_mono_ms)) continue;
    total += weight(n.score.total, factors_for(n, now_mono_ms, false));
    ++cand;
  }
  r.prob = total > 0 ? mine / total : 0;
  r.candidates = cand;
  const auto* c = ledger_.node(node_id);
  r.timeout_count = c != nullptr ? c->timeout_count : 0;
  return r;
}

std::optional<StateChange> NodeTable::end_rescue(const std::string& node_id, int64_t now_mono_ms,
                                                 int64_t now_wall_ms) {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = nodes_.find(node_id);
  if (it == nodes_.end() || it->second.br.mode != NodeMode::kRescue) return std::nullopt;
  NodeState& n = it->second;
  std::vector<StateChange> changes;
  if (fresh(n, now_mono_ms)) {
    set_mode(n, NodeMode::kSlowStart, "", now_mono_ms, now_wall_ms, &changes);
  } else {
    set_mode(n, NodeMode::kEvicted, "lost", now_mono_ms, now_wall_ms, &changes);
  }
  return changes.back();
}

std::optional<NodeMode> NodeTable::mode(const std::string& node_id) const {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = nodes_.find(node_id);
  if (it == nodes_.end()) return std::nullopt;
  return it->second.br.mode;
}

double NodeTable::f_slow(const std::string& node_id, int64_t now_mono_ms) const {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = nodes_.find(node_id);
  if (it == nodes_.end()) return 1;
  return slow_factor(it->second.br, now_mono_ms, opt_.breaker);
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
