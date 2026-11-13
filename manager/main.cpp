// manager：接收 Worker 上报，维护节点表与分数，处理 Allocate，写 Redis 热数据。
// 用法：manager [-c config/manager.yaml]
// Redis 密码从环境变量 LM_REDIS_PASSWORD 读取（与 deploy/.env 的 REDIS_PASSWORD 对应）；
// 环境变量 LM_REDIS_URI 非空时覆盖配置里的 redis.uri。

#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include <grpcpp/grpcpp.h>
#include <spdlog/spdlog.h>

#include "api/scheduler_service.h"
#include "ingest/collector_service.h"
#include "ingest/node_table.h"
#include "lm/clock.h"
#include "lm/config.h"
#include "lm/log.h"
#include "scheduler_loop.h"
#include "storage/redis_writer.h"

namespace {

std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop.store(true); }

void usage() {
  std::cerr << "用法: manager [-c <config.yaml>]\n"
               "  -c, --config  配置文件，默认 config/manager.yaml\n"
               "环境变量: LM_REDIS_PASSWORD  Redis 密码\n"
               "          LM_REDIS_URI       覆盖配置中的 redis.uri\n";
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

  lm::manager::CollectorService collector(table, queue);
  lm::manager::SchedulerService scheduler(table, {});
  const std::string listen = cfg.get<std::string>("listen", "0.0.0.0:50051");
  grpc::ServerBuilder builder;
  int bound_port = 0;
  builder.AddListeningPort(listen, grpc::InsecureServerCredentials(), &bound_port);
  builder.RegisterService(&collector);
  builder.RegisterService(&scheduler);
  std::unique_ptr<grpc::Server> server = builder.BuildAndStart();
  if (!server || bound_port == 0) {
    spdlog::critical("无法监听 {}", listen);
    return 1;
  }

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);
  writer.start();
  lm::manager::SchedulerLoop loop(table, {});
  loop.start();
  spdlog::info("manager 启动，gRPC 监听 {}，Redis {}", listen, ropt.uri);

  auto next_report = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (!g_stop.load()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    if (std::chrono::steady_clock::now() >= next_report) {
      const auto c = table.counts(lm::mono_ms());
      const auto t = table.inflight_totals();
      spdlog::info(
          "节点 在线 {}/{}，累计接受 {} 条，乱序丢弃 {} 条，存储队列 {}（丢弃 {}），Redis 错误 {}，"
          "分配 {} 次，拒绝 {} 次，在途 {}，作废 {}，未确认 {}，执行超时 {}，迟到事件 {}",
          c.online, c.total, collector.accepted(), collector.stale(), queue.size(), queue.dropped(),
          writer.write_errors(), scheduler.allocated(), scheduler.rejected(), t.inflight, t.voided,
          t.unconfirmed, t.exec_timeouts, t.late_events);
      next_report += std::chrono::seconds(10);
    }
  }

  spdlog::info("收到退出信号，正在停止");
  server->Shutdown(std::chrono::system_clock::now() + std::chrono::seconds(2));
  loop.stop();
  writer.stop();
  return 0;
}
