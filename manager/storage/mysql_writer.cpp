#include "mysql_writer.h"

#include <cmath>
#include <ctime>

#include <spdlog/spdlog.h>

#include "json_util.h"
#include "lm/clock.h"

namespace lm::manager {

namespace {

// 数值字面量：%.6g，NaN/Inf 写 NULL
std::string sql_num(double v) { return std::isfinite(v) ? json_num(v) : "NULL"; }
std::string num_or_null(const std::optional<double>& v) { return v ? sql_num(*v) : "NULL"; }
std::string ts_or_null(const std::optional<int64_t>& v) {
  return v ? "'" + mysql_datetime(*v) + "'" : "NULL";
}

}  // namespace

std::string mysql_datetime(int64_t wall_ms, bool millis) {
  const std::time_t secs = static_cast<std::time_t>(wall_ms / 1000);
  std::tm tm{};
  gmtime_r(&secs, &tm);
  char buf[40];
  std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
  std::string out = buf;
  if (millis) {
    char ms[8];
    std::snprintf(ms, sizeof(ms), ".%03d", static_cast<int>(wall_ms % 1000));
    out += ms;
  }
  return out;
}

MysqlWriter::MysqlWriter(MysqlQueue& queue, Options opt)
    : queue_(queue), opt_(std::move(opt)), db_(opt_.db), agg_(opt_.bucket_ms, opt_.grace_ms) {}

MysqlWriter::~MysqlWriter() { stop(); }

void MysqlWriter::start() {
  if (running_.exchange(true)) return;
  thread_ = std::thread([this] { run(); });
}

void MysqlWriter::stop() {
  if (!running_.exchange(false)) return;
  queue_.close();
  if (thread_.joinable()) thread_.join();
}

void MysqlWriter::consume(std::vector<MysqlItem> items) {
  for (auto& it : items) {
    if (auto* p = std::get_if<MetricPoint>(&it)) {
      agg_.add(*p);
    } else if (auto* n = std::get_if<NodeRow>(&it)) {
      if (nodes_.size() < opt_.max_pending_rows) {
        nodes_.push_back(std::move(*n));
      } else {
        ++local_dropped_;
      }
    } else if (auto* e = std::get_if<NodeEvent>(&it)) {
      if (events_.size() < opt_.max_pending_rows) {
        events_.push_back(std::move(*e));
      } else {
        ++local_dropped_;
      }
    } else if (auto* t = std::get_if<TaskChange>(&it)) {
      tasks_.add(*t);
    }
  }
}

std::string MysqlWriter::sql_metrics(const std::vector<MetricsRow>& rows) const {
  std::string sql =
      "INSERT INTO node_metrics_5m (node_id, bucket_start, cpu_avg, cpu_max, runq_p99_max_us, "
      "mem_avail_min, net_rtt_avg_ms, net_timeout_rate, net_retrans_rate, score_avg, samples) VALUES ";
  bool first = true;
  for (const auto& r : rows) {
    if (!first) sql += ',';
    first = false;
    sql += "(" + db_.quote(r.node_id) + ",'" + mysql_datetime(r.bucket_start_ms, false) + "'," +
           sql_num(r.cpu_avg) + "," + sql_num(r.cpu_max) + "," + num_or_null(r.runq_p99_max_us) +
           "," + sql_num(r.mem_avail_min) + "," + num_or_null(r.net_rtt_avg_ms) + "," +
           num_or_null(r.net_timeout_rate) + "," + num_or_null(r.net_retrans_rate) + "," +
           sql_num(r.score_avg) + "," + std::to_string(r.samples) + ")";
  }
  sql +=
      " ON DUPLICATE KEY UPDATE cpu_avg=VALUES(cpu_avg), cpu_max=VALUES(cpu_max), "
      "runq_p99_max_us=VALUES(runq_p99_max_us), mem_avail_min=VALUES(mem_avail_min), "
      "net_rtt_avg_ms=VALUES(net_rtt_avg_ms), net_timeout_rate=VALUES(net_timeout_rate), "
      "net_retrans_rate=VALUES(net_retrans_rate), score_avg=VALUES(score_avg), samples=VALUES(samples)";
  return sql;
}

std::string MysqlWriter::sql_events(const std::vector<NodeEvent>& evs) const {
  std::string sql = "INSERT INTO node_event (node_id, ts, kind, detail) VALUES ";
  bool first = true;
  for (const auto& e : evs) {
    if (!first) sql += ',';
    first = false;
    // detail 加上 event_id（stream:events 的 entry id）
    std::string detail = e.detail.empty() ? "{}" : e.detail;
    const std::string id_field = "\"event_id\":" + json_str(e.id);
    detail = detail == "{}" ? "{" + id_field + "}" : "{" + id_field + "," + detail.substr(1);
    sql += "(" + db_.quote(e.node_id) + ",'" + mysql_datetime(e.ts_ms) + "'," + db_.quote(e.kind) +
           "," + db_.quote(detail) + ")";
  }
  return sql;
}

std::string MysqlWriter::sql_nodes(const std::vector<NodeRow>& nodes) const {
  std::string sql = "INSERT INTO node (node_id, addr, ncpu, first_seen) VALUES ";
  bool first = true;
  for (const auto& n : nodes) {
    if (!first) sql += ',';
    first = false;
    sql += "(" + db_.quote(n.node_id) + "," + db_.quote(n.addr) + "," + std::to_string(n.ncpu) +
           ",'" + mysql_datetime(n.first_seen_ms, false) + "')";
  }
  sql += " ON DUPLICATE KEY UPDATE addr=VALUES(addr), ncpu=VALUES(ncpu)";
  return sql;
}

namespace {

// 新状态 v 是否覆盖旧状态 state 的 SQL 条件（与 task_state_overrides 一致）
const char* kRank =
    "(CASE %s WHEN 'allocated' THEN 0 WHEN 'sent' THEN 1 WHEN 'received' THEN 2 ELSE 3 END)";

std::string rank_of(const std::string& expr) {
  char buf[200];
  std::snprintf(buf, sizeof(buf), kRank, expr.c_str());
  return buf;
}

std::string state_update(const std::string& new_expr) {
  return "state = IF(" + rank_of(new_expr) + " > " + rank_of("state") + " OR (state = 'timeout' AND " +
         new_expr + " IN ('done','failed')), " + new_expr + ", state)";
}

}  // namespace

std::string MysqlWriter::sql_task_insert(const std::vector<TaskRow>& rows) const {
  std::string sql =
      "INSERT INTO task (task_id, node_id, input_bytes, state, t_alloc, t_sent, t_recv, t_done) VALUES ";
  bool first = true;
  for (const auto& r : rows) {
    if (!first) sql += ',';
    first = false;
    sql += "(" + db_.quote(r.task_id) + "," + db_.quote(r.node_id) + "," +
           std::to_string(r.input_bytes) + ",'" + task_state_name(r.state) + "'," +
           ts_or_null(r.t_alloc) + "," + ts_or_null(r.t_sent) + "," + ts_or_null(r.t_recv) + "," +
           ts_or_null(r.t_done) + ")";
  }
  sql += " ON DUPLICATE KEY UPDATE " + state_update("VALUES(state)") +
         ", t_sent=COALESCE(t_sent, VALUES(t_sent)), t_recv=COALESCE(t_recv, VALUES(t_recv)), "
         "t_done=COALESCE(t_done, VALUES(t_done))";
  return sql;
}

std::string MysqlWriter::sql_task_update(const TaskRow& r) const {
  const std::string v = std::string("'") + task_state_name(r.state) + "'";
  return "UPDATE task SET " + state_update(v) + ", t_sent=COALESCE(t_sent, " + ts_or_null(r.t_sent) +
         "), t_recv=COALESCE(t_recv, " + ts_or_null(r.t_recv) + "), t_done=COALESCE(t_done, " +
         ts_or_null(r.t_done) + ") WHERE task_id=" + db_.quote(r.task_id);
}

bool MysqlWriter::exec(const char* what, const std::string& sql, std::size_t rows) {
  if (db_.exec(sql)) {
    rows_.fetch_add(rows, std::memory_order_relaxed);
    return true;
  }
  errors_.fetch_add(1, std::memory_order_relaxed);
  uint64_t suppressed = 0;
  if (err_log_.allow(&suppressed)) {
    spdlog::warn("MySQL 写 {} 失败: {}（此前 10 秒内另有 {} 次失败未记录）", what, db_.error(),
                 suppressed);
  }
  return false;
}

void MysqlWriter::flush(int64_t now_wall_ms, int64_t now_mono_ms, bool all) {
  if (!opt_.db.enabled) {
    agg_.take_all();
    tasks_.take();
    events_.clear();
    nodes_.clear();
    retry_buckets_.clear();
    return;
  }
  const bool have_work = !nodes_.empty() || !events_.empty() || tasks_.size() > 0 ||
                         agg_.open_buckets() > 0 || !retry_buckets_.empty();
  if (!have_work) return;
  if (!db_.ensure(now_mono_ms)) {
    uint64_t suppressed = 0;
    if (err_log_.allow(&suppressed)) {
      spdlog::warn("MySQL 不可用：{}（缓冲 事件 {}、任务 {}、未写的桶 {}）", db_.error(), events_.size(),
                   tasks_.size(), agg_.open_buckets() + retry_buckets_.size());
    }
    // 不可用时任务与事件缓冲按上限保留，聚合桶留在内存里等恢复
    return;
  }
  // 写失败时：连接已断开（db_.connected() 为假）就把这一批放回缓冲，重连后再写；
  // 连接还在说明是语句本身的错误，丢弃并计数，避免一条坏数据反复重试。
  auto lost = [this] { return !db_.connected(); };
  if (!nodes_.empty()) {
    if (!exec("node", sql_nodes(nodes_), nodes_.size()) && lost()) return;
    nodes_.clear();
  }
  if (!events_.empty()) {
    if (!exec("node_event", sql_events(events_), events_.size()) && lost()) return;
    events_.clear();
  }
  auto rows = tasks_.take();
  std::vector<TaskRow> inserts;
  for (std::size_t i = 0; i < rows.size(); ++i) {
    if (rows[i].has_alloc) {
      inserts.push_back(rows[i]);
    } else if (!exec("task", sql_task_update(rows[i]), 1) && lost()) {
      std::vector<TaskRow> rest(rows.begin() + static_cast<std::ptrdiff_t>(i), rows.end());
      rest.insert(rest.end(), inserts.begin(), inserts.end());
      tasks_.restore(std::move(rest));
      return;
    }
  }
  if (!inserts.empty() && !exec("task", sql_task_insert(inserts), inserts.size()) && lost()) {
    tasks_.restore(std::move(inserts));
    return;
  }
  auto buckets = all ? agg_.take_all() : agg_.take_finished(now_wall_ms);
  buckets.insert(buckets.begin(), retry_buckets_.begin(), retry_buckets_.end());
  retry_buckets_.clear();
  if (!buckets.empty() && !exec("node_metrics_5m", sql_metrics(buckets), buckets.size()) && lost()) {
    retry_buckets_ = std::move(buckets);
  }
}

void MysqlWriter::run() {
  if (opt_.db.enabled && !MysqlClient::compiled_in()) {
    spdlog::warn("MySQL 冷数据：未编译 MySQL 支持，不写入");
  }
  auto next = std::chrono::steady_clock::now() + opt_.flush_interval;
  while (running_.load(std::memory_order_relaxed)) {
    consume(queue_.pop_batch(1024, std::chrono::milliseconds(100)));
    if (std::chrono::steady_clock::now() >= next) {
      flush(lm::wall_ms(), lm::mono_ms(), false);
      next = std::chrono::steady_clock::now() + opt_.flush_interval;
      dropped_.store(local_dropped_ + queue_.dropped() + tasks_.dropped());
    }
  }
  consume(queue_.pop_batch(1 << 20, std::chrono::milliseconds(0)));
  flush(lm::wall_ms(), lm::mono_ms(), true);  // 退出前写出未结束的桶
}

}  // namespace lm::manager
