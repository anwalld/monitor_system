#include "inflight_ledger.h"

#include <algorithm>
#include <cmath>

namespace lm::manager {

namespace {

[[noreturn]] void fail(const std::string& key, const std::string& why) {
  throw lm::ConfigError("配置项 " + key + " 不合法：" + why);
}

}  // namespace

void InflightParams::validate() const {
  if (ack_timeout_ms <= 0) fail("inflight.ack_timeout_s", "必须大于 0");
  if (exec_timeout_ms <= 0) fail("inflight.exec_timeout_s", "必须大于 0");
  if (!(lambda >= 0)) fail("inflight.lambda", "不能为负");
  if (!(timeout_half_life_s > 0)) fail("inflight.timeout_half_life_s", "必须大于 0");
  if (!(baseline_tau_s > 0)) fail("inflight.baseline_tau_s", "必须大于 0");
  if (!(base_tasks_floor > 0)) fail("inflight.base_tasks_floor", "必须大于 0");
  if (!(base_bytes_floor > 0)) fail("inflight.base_bytes_floor", "必须大于 0");
}

InflightParams InflightParams::from_config(const lm::Config& cfg) {
  InflightParams p;
  p.ack_timeout_ms = static_cast<int64_t>(cfg.get<double>("inflight.ack_timeout_s", 30) * 1000);
  p.exec_timeout_ms = static_cast<int64_t>(cfg.get<double>("inflight.exec_timeout_s", 300) * 1000);
  p.lambda = cfg.get<double>("inflight.lambda", p.lambda);
  p.timeout_half_life_s = cfg.get<double>("inflight.timeout_half_life_s", p.timeout_half_life_s);
  p.baseline_tau_s = cfg.get<double>("inflight.baseline_tau_s", p.baseline_tau_s);
  p.base_tasks_floor = cfg.get<double>("inflight.base_tasks_floor", p.base_tasks_floor);
  p.base_bytes_floor = cfg.get<double>("inflight.base_bytes_floor", p.base_bytes_floor);
  p.validate();
  return p;
}

bool InflightLedger::add(const std::string& task_id, const std::string& node_id,
                         uint64_t input_bytes, int64_t now_ms) {
  auto [it, inserted] = tasks_.try_emplace(task_id);
  if (!inserted) return false;
  Task& t = it->second;
  t.node_id = node_id;
  t.bytes = input_bytes;
  t.state = State::kAllocated;
  t.deadline = now_ms + p_.ack_timeout_ms;
  t.it = ack_.insert(ack_.end(), task_id);
  NodeCounters& n = nodes_[node_id];
  ++n.assigned_total;
  ++n.total;
  ++n.inflight_tasks;
  n.inflight_bytes += static_cast<int64_t>(input_bytes);
  return true;
}

InflightLedger::MarkResult InflightLedger::mark_sent(const std::string& task_id, int64_t now_ms) {
  auto it = tasks_.find(task_id);
  if (it == tasks_.end()) return MarkResult::kNotFound;
  Task& t = it->second;
  if (t.state == State::kAllocated) {
    t.state = State::kSent;
    t.deadline = now_ms + p_.ack_timeout_ms;
    ack_.splice(ack_.end(), ack_, t.it);  // 移到队尾，迭代器仍有效
  }
  return MarkResult::kOk;
}

void InflightLedger::release(Task& t) {
  NodeCounters& n = nodes_[t.node_id];
  --n.inflight_tasks;
  n.inflight_bytes -= static_cast<int64_t>(t.bytes);
}

void InflightLedger::erase(std::unordered_map<std::string, Task>::iterator it) {
  Task& t = it->second;
  (t.state == State::kRunning ? exec_ : ack_).erase(t.it);
  tasks_.erase(it);
}

InflightLedger::EventResult InflightLedger::on_event(const std::string& task_id,
                                                     lm::TaskEvent::Kind kind, int64_t now_ms) {
  auto it = tasks_.find(task_id);
  if (it == tasks_.end()) {
    ++late_events_;
    return EventResult::kLate;
  }
  Task& t = it->second;
  switch (kind) {
    case lm::TaskEvent::RECEIVED:
      if (t.state == State::kRunning) return EventResult::kIgnored;
      ack_.erase(t.it);
      t.state = State::kRunning;
      t.deadline = now_ms + p_.exec_timeout_ms;
      t.it = exec_.insert(exec_.end(), task_id);
      return EventResult::kApplied;
    case lm::TaskEvent::DONE:
    case lm::TaskEvent::FAILED:
      release(t);
      erase(it);
      return EventResult::kApplied;
    default:
      return EventResult::kIgnored;
  }
}

void InflightLedger::tick(int64_t now_ms, std::vector<Expired>* out) {
  while (!ack_.empty()) {
    auto it = tasks_.find(ack_.front());
    Task& t = it->second;
    if (t.deadline > now_ms) break;
    NodeCounters& n = nodes_[t.node_id];
    const bool sent = t.state == State::kSent;
    if (sent) {
      ++n.unconfirmed;
    } else {
      ++n.voided;
    }
    --n.total;
    release(t);
    if (out != nullptr) {
      out->push_back({it->first, t.node_id,
                      sent ? Expired::Kind::kUnconfirmed : Expired::Kind::kVoided});
    }
    erase(it);
  }
  while (!exec_.empty()) {
    auto it = tasks_.find(exec_.front());
    Task& t = it->second;
    if (t.deadline > now_ms) break;
    NodeCounters& n = nodes_[t.node_id];
    ++n.exec_timeouts;
    n.timeout_count += 1;
    release(t);
    if (out != nullptr) out->push_back({it->first, t.node_id, Expired::Kind::kExecTimeout});
    erase(it);
  }
}

void InflightLedger::tick_second(double dt_s) {
  const double alpha = 1 - std::exp(-dt_s / p_.baseline_tau_s);
  const double decay = std::pow(0.5, dt_s / p_.timeout_half_life_s);
  for (auto& [id, n] : nodes_) {
    n.base_tasks += alpha * (static_cast<double>(n.inflight_tasks) - n.base_tasks);
    n.base_bytes += alpha * (static_cast<double>(n.inflight_bytes) - n.base_bytes);
    n.timeout_count *= decay;
  }
}

Factors InflightLedger::factors(const std::string& node_id) const {
  Factors f;
  auto it = nodes_.find(node_id);
  if (it == nodes_.end()) return f;
  const NodeCounters& n = it->second;
  const double r = std::max(
      static_cast<double>(n.inflight_tasks) / std::max(n.base_tasks, p_.base_tasks_floor),
      static_cast<double>(n.inflight_bytes) / std::max(n.base_bytes, p_.base_bytes_floor));
  f.inflight = 1.0 / (1.0 + std::max(0.0, r - 1.0));
  f.timeout = std::exp(-p_.lambda * n.timeout_count);
  return f;
}

InflightSnapshot InflightLedger::snapshot(const std::string& node_id) const {
  InflightSnapshot s;
  auto it = nodes_.find(node_id);
  if (it == nodes_.end()) return s;
  const NodeCounters& n = it->second;
  const Factors f = factors(node_id);
  s.inflight_tasks = n.inflight_tasks;
  s.inflight_bytes = n.inflight_bytes;
  s.unconfirmed = n.unconfirmed;
  s.timeout_count = n.timeout_count;
  s.f_inflight = f.inflight;
  s.f_timeout = f.timeout;
  return s;
}

const InflightLedger::NodeCounters* InflightLedger::node(const std::string& node_id) const {
  auto it = nodes_.find(node_id);
  return it == nodes_.end() ? nullptr : &it->second;
}

std::optional<InflightLedger::State> InflightLedger::state(const std::string& task_id) const {
  auto it = tasks_.find(task_id);
  if (it == tasks_.end()) return std::nullopt;
  return it->second.state;
}

std::optional<int64_t> InflightLedger::deadline(const std::string& task_id) const {
  auto it = tasks_.find(task_id);
  if (it == tasks_.end()) return std::nullopt;
  return it->second.deadline;
}

InflightLedger::Totals InflightLedger::totals() const {
  Totals t;
  t.inflight = tasks_.size();
  t.late_events = late_events_;
  for (const auto& [id, n] : nodes_) {
    t.voided += n.voided;
    t.unconfirmed += n.unconfirmed;
    t.exec_timeouts += n.exec_timeouts;
  }
  return t;
}

}  // namespace lm::manager
