// manager：接收 Worker 上报，维护节点表与分数，处理 Allocate，接收 AI 预测（Advisor），写 Redis 热数据。
// 用法：manager [-c config/manager.yaml]
// Redis 密码从环境变量 LM_REDIS_PASSWORD 读取（与 deploy/.env 的 REDIS_PASSWORD 对应）；
// 环境变量 LM_REDIS_URI 非空时覆盖配置里的 redis.uri。
// 看板接口 token 从 LM_API_TOKEN 读取（覆盖 api.token）。
// MySQL（M7 冷数据）密码从 LM_MYSQL_PASSWORD 读取，LM_MYSQL_HOST 非空时覆盖 mysql.host。

#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include <grpcpp/grpcpp.h>
#include <spdlog/spdlog.h>

#include "api/advisor_service.h"
#include "api/scheduler_service.h"
#include "api/web_server.h"
#include "ingest/collector_service.h"
#include "ingest/node_table.h"
#include "lm/clock.h"
#include "lm/config.h"
#include "lm/log.h"
#include "scheduler_loop.h"
#include "storage/mysql_writer.h"
#include "storage/redis_writer.h"

namespace {

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop.store(true); }

void usage() {
  std::cerr << "用法: manager [-c <config.yaml>]\n"
               "  -c, --config  配置文件，默认 config/manager.yaml\n"
               "环境变量: LM_REDIS_PASSWORD  Redis 密码\n"
               "          LM_REDIS_URI       覆盖配置中的 redis.uri\n"
               "          LM_MYSQL_PASSWORD  MySQL 密码\n"
               "          LM_MYSQL_HOST / LM_MYSQL_PORT / LM_MYSQL_USER / LM_MYSQL_DATABASE\n"
               "                             覆盖配置中的 mysql 段\n"
               "          LM_API_TOKEN       看板接口 token（覆盖 api.token）\n";
}

}  // namespace

int main(int argc, char** argv) {
  std::string config_path = "config/manager.yaml";
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if ((a == "-c" || a == "--config") && i + 1 < argc) {
      config_path = argv[++i];
    } else if (a == "-h" || a == "--help") {
      usage();
      return 0;
    } else {
      usage();
      return 2;
    }
  }

  lm::Config cfg;
  try {
    cfg = lm::Config::load_file(config_path);
  } catch (const lm::ConfigError& e) {
    std::cerr << e.what() << "\n";
    return 2;
  }
  lm::init_logging("manager", cfg.get<std::string>("log_level", "info"));

  lm::manager::NodeTable::Options topt;
  try {
    topt.worker_port = static_cast<uint16_t>(cfg.get<int>("worker_port", 50052));
    topt.scoring = lm::manager::ScoringParams::from_config(cfg);
    topt.inflight = lm::manager::InflightParams::from_config(cfg);
    topt.breaker = lm::manager::BreakerParams::from_config(cfg);
    topt.advisor = lm::manager::AdvisorParams::from_config(cfg);
  } catch (const lm::ConfigError& e) {
    spdlog::critical("{}", e.what());
    return 2;
  }
  topt.online_window_ms = topt.scoring.lost_after_ms;
  lm::manager::NodeTable table(topt);
  lm::manager::StorageQueue queue(cfg.get<std::size_t>("storage.queue_capacity", 4096));

  lm::manager::RedisWriter::Options ropt;
  ropt.uri = cfg.get<std::string>("redis.uri", "tcp://127.0.0.1:6379");
  if (const char* uri = std::getenv("LM_REDIS_URI"); uri != nullptr && uri[0] != '\0') ropt.uri = uri;
  ropt.ts_retention_ms = cfg.get<int64_t>("redis.ts_retention_s", 3600) * 1000;
  if (const char* pw = std::getenv("LM_REDIS_PASSWORD")) ropt.password = pw;
  lm::manager::RedisWriter writer(queue, ropt);

  // MySQL 冷数据（M7）：独立线程与队列，写失败不影响调度和 Redis
  lm::manager::MysqlWriter::Options mopt;
  mopt.db.enabled = cfg.get<bool>("mysql.enabled", true);
  mopt.db.host = cfg.get<std::string>("mysql.host", "127.0.0.1");
  if (const char* h = std::getenv("LM_MYSQL_HOST"); h != nullptr && h[0] != '\0') mopt.db.host = h;
  mopt.db.port = static_cast<unsigned>(cfg.get<int>("mysql.port", 3306));
  mopt.db.user = cfg.get<std::string>("mysql.user", "lm");
  mopt.db.database = cfg.get<std::string>("mysql.database", "lm");
  if (const char* pw = std::getenv("LM_MYSQL_PASSWORD")) mopt.db.password = pw;
  // compose 用 .env 里的端口、用户、库名覆盖
  if (const char* v = std::getenv("LM_MYSQL_PORT"); v != nullptr && v[0] != '\0') {
    mopt.db.port = static_cast<unsigned>(std::strtoul(v, nullptr, 10));
  }
  if (const char* v = std::getenv("LM_MYSQL_USER"); v != nullptr && v[0] != '\0') mopt.db.user = v;
  if (const char* v = std::getenv("LM_MYSQL_DATABASE"); v != nullptr && v[0] != '\0') {
    mopt.db.database = v;
  }
  lm::manager::MysqlQueue mqueue(cfg.get<std::size_t>("mysql.queue_capacity", 16384));
  lm::manager::MysqlWriter mwriter(mqueue, mopt);
  lm::manager::TaskSink task_sink;
  if (mopt.db.enabled) {
    writer.set_mysql(&mqueue);
    task_sink = [&mqueue](lm::manager::TaskChange c) { mqueue.push(std::move(c)); };
  }

  lm::manager::CollectorService collector(table, queue);
  lm::manager::SchedulerService::Options sopt;
  sopt.allow_policy = cfg.get<bool>("allocation.allow_policy", true);
  lm::manager::SchedulerService scheduler(table, sopt);
  lm::manager::AdvisorService advisor(table);  // AI 负载预测（M9）
  collector.set_task_sink(task_sink);
  scheduler.set_task_sink(task_sink);
  const std::string listen = cfg.get<std::string>("listen", "0.0.0.0:50051");
  grpc::ServerBuilder builder;
  int bound_port = 0;
  builder.AddListeningPort(listen, grpc::InsecureServerCredentials(), &bound_port);
  builder.RegisterService(&collector);
  builder.RegisterService(&scheduler);
  builder.RegisterService(&advisor);
  std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
  if (!server || bound_port == 0) {
    spdlog::critical("无法监听 {}", listen);
    return 1;
  }

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  writer.start();
  mwriter.start();
  // 状态变化：打日志，并推一条只含状态的记录给存储线程（失联节点也能在 Redis 里看到状态）
  lm::manager::StateSink sink = [&queue](const lm::manager::StateChange& c) {
    if (c.from == c.to) return;  // 新节点首次登记已随样本写入
    const char* to = lm::manager::mode_name(c.to);
    if (c.to == lm::manager::NodeMode::kEvicted) {
      spdlog::warn("节点 {} 状态 {} → {}（{}）", c.node_id, lm::manager::mode_name(c.from), to,
                   c.reason);
    } else {
      spdlog::info("节点 {} 状态 {} → {}", c.node_id, lm::manager::mode_name(c.from), to);
    }
    lm::manager::AcceptedSample a;
    a.state_only = true;
    a.node_id = c.node_id;
    a.mode = c.to;
    a.mode_reason = c.reason;
    a.mode_since_wall_ms = c.since_wall_ms;
    a.f_slow = c.f_slow;
    a.probe_fail_streak = c.probe_fail_streak;
    a.events = lm::manager::events_from_change(c);
    queue.push(std::move(a));
  };
  // 急救结束：只含事件的记录，先于随后的状态变化入队（同一线程，顺序保证）
  lm::manager::RescueEndSink end_sink = [&queue](const lm::manager::RescueEnd& e) {
    lm::manager::AcceptedSample a;
    a.events_only = true;
    a.node_id = e.node_id;
    a.events.push_back(lm::manager::event_from_rescue_end(e));
    queue.push(std::move(a));
  };
  lm::manager::RescueManager rescue(table, topt.breaker.rescue, sink, end_sink);
  lm::manager::SchedulerLoop loop(table, {}, sink, &rescue);
  loop.set_task_sink(task_sink);
  lm::manager::Prober prober(table, topt.breaker, sink);

  // 看板接口（M7）：REST + WebSocket，独立线程
  lm::manager::WebServer::Options wopt;
  wopt.enabled = cfg.get<bool>("api.enabled", true);
  {
    const std::string l = cfg.get<std::string>("api.listen", "0.0.0.0:8080");
    const auto colon = l.rfind(':');
    wopt.host = colon == std::string::npos ? "0.0.0.0" : l.substr(0, colon);
    wopt.port = static_cast<uint16_t>(std::stoi(colon == std::string::npos ? l : l.substr(colon + 1)));
  }
  wopt.token = cfg.get<std::string>("api.token", "");
  if (const char* t = std::getenv("LM_API_TOKEN"); t != nullptr && t[0] != '\0') wopt.token = t;
  wopt.threads = cfg.get<std::size_t>("api.threads", 2);
  wopt.redis_uri = ropt.uri;
  wopt.redis_password = ropt.password;
  lm::manager::WebServer web(table, wopt);
  loop.start();
  prober.start();
  web.start();
  spdlog::info("manager 启动，gRPC 监听 {}，Redis {}，MySQL {}", listen, ropt.uri,
               mopt.db.enabled ? mopt.db.host + ":" + std::to_string(mopt.db.port) : "关闭");

  auto next_report = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!g_stop.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    if (std::chrono::steady_clock::now() >= next_report) {
      const auto c = table.counts(lm::mono_ms());
      const auto t = table.inflight_totals();
      spdlog::info(
          "节点 在线 {}/{}（慢启动 {}、正常 {}、剔除 {}、急救 {}），累计接受 {} 条，乱序丢弃 {} 条，存储队列 {}（丢弃 {}），Redis 错误 {}，"
          "分配 {} 次，拒绝 {} 次，在途 {}，作废 {}，未确认 {}，执行超时 {}，迟到事件 {}，"
          "MySQL 写入 {} 行（错误 {}，队列 {}，丢弃 {}），预测批 {}（未知节点 {}）",
          c.online, c.total, c.slow_start, c.normal, c.evicted, c.rescue, collector.accepted(), collector.stale(), queue.size(), queue.dropped(),
          writer.write_errors(), scheduler.allocated(), scheduler.rejected(), t.inflight, t.voided,
          t.unconfirmed, t.exec_timeouts, t.late_events, mwriter.rows_written(), mwriter.errors(),
          mqueue.size(), mwriter.dropped(), advisor.batches(), advisor.unknown_nodes());
      next_report += std::chrono::seconds(10);
    }
  }

  spdlog::info("收到退出信号，正在停止");
  web.stop();
  server->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(2));
  prober.stop();
  loop.stop();
  writer.stop();   // 先停 Redis 写线程：它会把剩余事件转给 MySQL 队列
  mwriter.stop();  // 再停 MySQL：写出未结束的 5 分钟桶
  return 0;
}
