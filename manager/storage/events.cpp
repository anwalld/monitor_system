#include "events.h"

#include "json_util.h"

namespace lm::manager {

std::vector<NodeEvent> events_from_change(const StateChange& c, const std::string& addr,
                                          uint32_t ncpu) {
  NodeEvent e;
  e.node_id = c.node_id;
  e.ts_ms = c.since_wall_ms;
  const bool is_new = c.from == NodeMode::kSlowStart && c.to == NodeMode::kSlowStart;
  switch (c.to) {
    case NodeMode::kSlowStart: {
      e.kind = "slow_start";
      JsonObj d;
      if (is_new) {
        d.add_str("from", "new").add_str("addr", addr).add_int("ncpu", ncpu);
      } else {
        d.add_str("from", mode_name(c.from));  // evicted / rescue
      }
      e.detail = d.str();
      break;
    }
    case NodeMode::kNormal:
      e.kind = "normal";
      e.detail = "{}";
      break;
    case NodeMode::kEvicted:
      e.kind = "evict";
      e.detail = JsonObj().add_str("from", mode_name(c.from)).add_str("reason", c.reason).str();
      break;
    case NodeMode::kRescue:
      e.kind = "rescue_start";
      e.detail = c.detail_json.empty() ? "{}" : c.detail_json;
      break;
  }
  return {e};
}

NodeEvent event_from_rescue_end(const RescueEnd& r) {
  NodeEvent e;
  e.node_id = r.node_id;
  e.kind = "rescue_end";
  e.ts_ms = r.ts_ms;
  e.detail = JsonObj()
                 .add_str("result", r.result)
                 .add_int("ms", r.ms)
                 .add_int("batches_ok", r.batches_ok)
                 .add_int("batches_fail", r.batches_fail)
                 .add_int("sent", static_cast<long long>(r.sent))
                 .add_int("ok", static_cast<long long>(r.ok))
                 .add_int("timeouts", static_cast<long long>(r.timeouts))
                 .add_int("late", static_cast<long long>(r.late))
                 .add_int("events", static_cast<long long>(r.events))
                 .add("prob", r.prob)
                 .add("target", r.target)
                 .add("t_before", r.t_before)
                 .add("t_after", r.t_after)
                 .add_str("to", r.to)
                 .str();
  return e;
}

}  // namespace lm::manager
