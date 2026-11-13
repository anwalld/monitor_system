// worker：采集节点指标并每秒上报给 Manager。
// 用法：worker [-c config/worker.yaml] [--dry-run]
//   --dry-run  不连接 Manager，把每条样本以 JSON 打印到标准输出

#include <unistd.h>

#include <csignal>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#include <google/protobuf/util/json_util.h>
#include <grpcpp/grpcpp.h>
#include <spdlog/spdlog.h>

#include "collector/collector.h"
#include "collector/proc_readers.h"
#include "collector/runq_probe.h"
#include "collector/shm_cpu_source.h"
#include "lm/clock.h"
#include "lm/config.h"
#include "lm/log.h"
#include "reporter/reporter.h"

namespace {

std::atomic<bool> g_stop{false};

void on_signal(int) { g_stop.store(true); }

std::string default_node_id() {
  char host[256] = {};
  if (::gethostname(host, sizeof(host) - 1) != 0 || host[0] == '\0') return "worker";
  return host;
}

// 按 cpu_source 配置创建 CPU 来源：auto 优先共享页、失败回退 /proc；shm 失败返回空；proc 直接用 /proc。
std::unique_ptr<lm::worker::CpuSource> make_cpu_source(const std::string& mode) {
  if (mode == "proc") {
    spdlog::info("CPU 来源：/proc/stat（配置 cpu_source=proc）");
    return std::make_unique<lm::worker::ProcCpuSource>();
  }
  try {
    auto shm = std::make_unique<lm::worker::ShmCpuSource>();
    spdlog::info("CPU 来源：cpustat 共享页 {}（刷新周期 {}us）", shm->path(), shm->interval_us());
    return shm;
  } catch (const std::exception& e) {
    if (mode == "shm") {
      spdlog::critical("cpu_source=shm 但无法使用 cpustat 共享页：{}", e.what());
      return nullptr;
    }
    spdlog::warn("无法使用 cpustat 共享页（{}），回退到 /proc/stat", e.what());
    return std::make_unique<lm::worker::ProcCpuSource>();
  }
}

void usage() {
  std::cerr << "用法: worker [-c <config.yaml>] [--dry-run]\n"
               "  -c, --config  配置文件，默认 config/worker.yaml\n"
               "  --dry-run     不上报，把样本以 JSON 打印到标准输出\n";
}

}  // namespace

int main(int argc, char** argv) {
  std::string config_path = "config/worker.yaml";
  bool dry_run = false;
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if ((a == "-c" || a == "--config") && i + 1 < argc) {
      config_path = argv[++i];
    } else if (a == "--dry-run") {
      dry_run = true;
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

  lm::init_logging("worker", cfg.get<std::string>("log_level", "info"));

  const std::string cpu_mode = cfg.get<std::string>("cpu_source", "auto");
  if (cpu_mode != "auto" && cpu_mode != "shm" && cpu_mode != "proc") {
    spdlog::critical("配置项 cpu_source 取值无效：{}（可选 auto、shm、proc）", cpu_mode);
    return 2;
  }

  const std::string runq_mode = cfg.get<std::string>("runq", "auto");
  if (runq_mode != "auto" && runq_mode != "on" && runq_mode != "off") {
    spdlog::critical("配置项 runq 取值无效：{}（可选 auto、on、off）", runq_mode);
    return 2;
  }

  if (::access("/proc/stat", R_OK) != 0) {
    spdlog::critical("需要 Linux /proc（找不到 /proc/stat），worker 只能在 Linux 上运行");
    return 1;
  }

  std::string node_id = cfg.get<std::string>("node_id", "");
  if (node_id.empty()) node_id = default_node_id();
  const auto collect_interval =
      std::chrono::milliseconds(cfg.get<int>("collect_interval_ms", 1000));
  lm::worker::Reporter::Options ropt;
  ropt.interval = std::chrono::milliseconds(cfg.get<int>("report_interval_ms", 1000));
  ropt.deadline = std::chrono::milliseconds(cfg.get<int>("report_deadline_ms", 500));
  const std::string manager = cfg.get<std::string>("manager_addr", "127.0.0.1:50051");

  std::signal(SIGINT, on_signal);
  std::signal(SIGTERM, on_signal);

  auto cpu_source = make_cpu_source(cpu_mode);
  if (!cpu_source) return 1;

  // 调度延迟（eBPF runqlat）：auto 失败只警告不填字段；on 失败退出；off 不加载
  std::shared_ptr<lm::worker::RunqProbe> runq;
  if (runq_mode == "off") {
    spdlog::info("调度延迟采集：未启用（配置 runq=off）");
  } else {
    try {
      runq = std::make_shared<lm::worker::RunqProbe>();
      spdlog::info("调度延迟采集：已启用（eBPF runqlat）");
    } catch (const std::exception& e) {
      if (runq_mode == "on") {
        spdlog::critical("runq=on 但无法启用调度延迟采集：{}", e.what());
        return 1;
      }
      spdlog::warn("调度延迟采集：未启用（{}），样本不含调度延迟字段", e.what());
    }
  }

  lm::worker::SampleRing ring;
  auto mem = std::make_shared<lm::worker::MemReader>();
  auto disk = std::make_shared<lm::worker::DiskReader>();
  lm::worker::MetricReaders readers{
      [mem] { return mem->read_avail_ratio(); },
      [disk](int64_t now) { return disk->read_util(now); },
      nullptr,
  };
  if (runq) {
    auto window = std::make_shared<lm::worker::RunqWindow>();
    readers.runq = [runq, window] {
      lm::worker::RunqResult r;
      lm::worker::RunqCumulative cum;
      if (!runq->read(cum)) {
        r.status = lm::worker::RunqResult::kFailed;
        return r;
      }
      if (auto st = window->update(cum)) {
        r.status = lm::worker::RunqResult::kOk;
        r.stats = *st;
      } else {
        r.status = lm::worker::RunqResult::kBaseline;
      }
      return r;
    };
  }
  lm::worker::Collector collector(node_id, std::move(cpu_source), std::move(readers), ring,
                                  collect_interval);

  std::unique_ptr<lm::worker::Reporter> reporter;
  if (dry_run) {
    reporter = std::make_unique<lm::worker::Reporter>(
        ring,
        [](const lm::Sample& s) {
          std::string json;
          google::protobuf::util::JsonPrintOptions opt;
          opt.preserve_proto_field_names = true;
          (void)google::protobuf::util::MessageToJsonString(s, &json, opt);
          std::cout << json << std::endl;
        },
        ropt);
    spdlog::info("dry-run 模式，node_id={}，样本打印到标准输出", node_id);
  } else {
    grpc::ChannelArguments args;
    args.SetInt(GRPC_ARG_KEEPALIVE_TIME_MS, 10000);
    // gRPC 默认重连退避会增长到数十秒（实测 Manager 重启后 26 秒才恢复上报），
    // 会让健康节点在 Manager 侧超过 3 秒失联阈值。上报每秒一次，把重连间隔压到 1 秒以内。
    args.SetInt(GRPC_ARG_INITIAL_RECONNECT_BACKOFF_MS, 500);
    args.SetInt(GRPC_ARG_MIN_RECONNECT_BACKOFF_MS, 500);
    args.SetInt(GRPC_ARG_MAX_RECONNECT_BACKOFF_MS, 1000);
    auto channel = grpc::CreateCustomChannel(manager, grpc::InsecureChannelCredentials(), args);
    reporter = std::make_unique<lm::worker::Reporter>(ring, channel, ropt);
    spdlog::info("worker 启动，node_id={}，manager={}", node_id, manager);
  }

  collector.start();
  reporter->start();
  while (!g_stop.load()) std::this_thread::sleep_for(std::chrono::milliseconds(200));

  spdlog::info("收到退出信号，正在停止");
  reporter->stop();
  collector.stop();
  spdlog::info("已停止：采集 {} 条，上报成功 {} 次、失败 {} 次", collector.samples_published(),
               reporter->sent_ok(), reporter->sent_failed());
  return 0;
}
