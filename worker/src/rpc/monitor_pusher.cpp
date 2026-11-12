#include "rpc/monitor_pusher.h"

#include <iostream>
#include <chrono>

namespace monitor {

MonitorPusher::MonitorPusher(const std::string& manager_address,
                             int interval_seconds)
    : manager_address_(manager_address),
      interval_seconds_(interval_seconds),
      running_(false) {
  // 创建 gRPC channel 和 stub
  auto channel = grpc::CreateChannel(manager_address,
                                     grpc::InsecureChannelCredentials());
  stub_ = monitor::proto::GrpcManager::NewStub(channel);

  // 创建指标采集器
  collector_ = std::make_unique<MetricCollector>();
}

MonitorPusher::~MonitorPusher() {
  Stop();
}

void MonitorPusher::Start() {
  if (running_) {
    return;
  }
  running_ = true;
  thread_ = std::make_unique<std::thread>(&MonitorPusher::PushLoop, this);
  std::cout << "MonitorPusher started, pushing to " << manager_address_
            << " every " << interval_seconds_ << " seconds" << std::endl;
}

void MonitorPusher::Stop() {
  running_ = false;
  if (thread_ && thread_->joinable()) {
    thread_->join();
  }
}

void MonitorPusher::PushLoop() {
  while (running_) {
    if (!PushOnce()) {
      std::cerr << "Failed to push monitor data to " << manager_address_
                << std::endl;
    }

    // 等待指定间隔
    for (int i = 0; i < interval_seconds_ && running_; ++i) {
      std::this_thread::sleep_for(std::chrono::seconds(1));
    }
  }
}

bool MonitorPusher::PushOnce() {
  // 采集监控数据
  monitor::proto::MonitorInfo info;
  collector_->CollectAll(&info);

  // 打印采集到的所有指标
  std::cout << "\n================== Collected Metrics ==================" << std::endl;
  
  // 主机信息
  if (info.has_host_info()) {
    std::cout << "[Host] Hostname: " << info.host_info().hostname()
              << ", IP: " << info.host_info().ip_address() << std::endl;
  }
  
  // CPU 统计信息 - 所有核心
  std::cout << "\n--- CPU Statistics ---" << std::endl;
  for (int i = 0; i < info.cpu_stat_size(); ++i) {
    const auto& cpu = info.cpu_stat(i);
    std::cout << "[" << cpu.cpu_name() << "] "
              << "Total: " << cpu.cpu_percent() << "%, "
              << "User: " << cpu.usr_percent() << "%, "
              << "System: " << cpu.system_percent() << "%, "
              << "Nice: " << cpu.nice_percent() << "%, "
              << "Idle: " << cpu.idle_percent() << "%, "
              << "IOWait: " << cpu.io_wait_percent() << "%, "
              << "IRQ: " << cpu.irq_percent() << "%, "
              << "SoftIRQ: " << cpu.soft_irq_percent() << "%" << std::endl;
  }
  
  // CPU 负载
  if (info.has_cpu_load()) {
    std::cout << "\n--- CPU Load ---" << std::endl;
    std::cout << "[Load] 1min: " << info.cpu_load().load_avg_1()
              << ", 5min: " << info.cpu_load().load_avg_3()
              << ", 15min: " << info.cpu_load().load_avg_15() << std::endl;
  }
  
  // 内存信息 - 所有字段
  if (info.has_mem_info()) {
    const auto& mem = info.mem_info();
    std::cout << "\n--- Memory Info ---" << std::endl;
    std::cout << "[Memory] Used: " << mem.used_percent() << "%" << std::endl;
    std::cout << "  Total: " << mem.total() << " MB, "
              << "Free: " << mem.free() << " MB, "
              << "Avail: " << mem.avail() << " MB" << std::endl;
    std::cout << "  Buffers: " << mem.buffers() << " MB, "
              << "Cached: " << mem.cached() << " MB, "
              << "SwapCached: " << mem.swap_cached() << " MB" << std::endl;
    std::cout << "  Active: " << mem.active() << " MB, "
              << "Inactive: " << mem.inactive() << " MB" << std::endl;
    std::cout << "  ActiveAnon: " << mem.active_anon() << " MB, "
              << "InactiveAnon: " << mem.inactive_anon() << " MB" << std::endl;
    std::cout << "  ActiveFile: " << mem.active_file() << " MB, "
              << "InactiveFile: " << mem.inactive_file() << " MB" << std::endl;
    std::cout << "  Dirty: " << mem.dirty() << " MB, "
              << "Writeback: " << mem.writeback() << " MB" << std::endl;
    std::cout << "  AnonPages: " << mem.anon_pages() << " MB, "
              << "Mapped: " << mem.mapped() << " MB" << std::endl;
    std::cout << "  KReclaimable: " << mem.kreclaimable() << " MB, "
              << "SReclaimable: " << mem.sreclaimable() << " MB, "
              << "SUnreclaim: " << mem.sunreclaim() << " MB" << std::endl;
  }
  
  // 网络信息 - 所有网卡所有字段
  if (info.net_info_size() > 0) {
    std::cout << "\n--- Network Info ---" << std::endl;
    for (int i = 0; i < info.net_info_size(); ++i) {
      const auto& net = info.net_info(i);
      std::cout << "[" << net.name() << "]" << std::endl;
      std::cout << "  Recv: " << net.rcv_rate() << " B/s ("
                << net.rcv_packets_rate() << " pkt/s)" << std::endl;
      std::cout << "  Send: " << net.send_rate() << " B/s ("
                << net.send_packets_rate() << " pkt/s)" << std::endl;
      std::cout << "  Errors(in/out): " << net.err_in() << "/" << net.err_out()
                << ", Drops(in/out): " << net.drop_in() << "/" << net.drop_out() << std::endl;
    }
  }
  
  // 磁盘信息 - 所有磁盘所有字段
  if (info.disk_info_size() > 0) {
    std::cout << "\n--- Disk Info ---" << std::endl;
    for (int i = 0; i < info.disk_info_size(); ++i) {
      const auto& disk = info.disk_info(i);
      std::cout << "[" << disk.name() << "]" << std::endl;
      std::cout << "  Read: " << disk.read_bytes_per_sec() / 1024.0 << " KB/s, "
                << "IOPS: " << disk.read_iops() << ", "
                << "Latency: " << disk.avg_read_latency_ms() << " ms" << std::endl;
      std::cout << "  Write: " << disk.write_bytes_per_sec() / 1024.0 << " KB/s, "
                << "IOPS: " << disk.write_iops() << ", "
                << "Latency: " << disk.avg_write_latency_ms() << " ms" << std::endl;
      std::cout << "  Util: " << disk.util_percent() << "%, "
                << "IO_InProgress: " << disk.io_in_progress() << std::endl;
      std::cout << "  Reads: " << disk.reads() << ", "
                << "Writes: " << disk.writes() << ", "
                << "SectorsRead: " << disk.sectors_read() << ", "
                << "SectorsWritten: " << disk.sectors_written() << std::endl;
    }
  }
  
  // 软中断信息 - 所有 CPU 核心
  if (info.soft_irq_size() > 0) {
    std::cout << "\n--- SoftIRQ Info ---" << std::endl;
    for (int i = 0; i < info.soft_irq_size(); ++i) {
      const auto& sirq = info.soft_irq(i);
      std::cout << "[" << sirq.cpu() << "] "
                << "HI: " << sirq.hi() << ", "
                << "TIMER: " << sirq.timer() << ", "
                << "NET_TX: " << sirq.net_tx() << ", "
                << "NET_RX: " << sirq.net_rx() << ", "
                << "BLOCK: " << sirq.block() << ", "
                << "IRQ_POLL: " << sirq.irq_poll() << ", "
                << "TASKLET: " << sirq.tasklet() << ", "
                << "SCHED: " << sirq.sched() << ", "
                << "HRTIMER: " << sirq.hrtimer() << ", "
                << "RCU: " << sirq.rcu() << std::endl;
    }
  }
  
  std::cout << "========================================================\n" << std::endl;

  // 推送数据
  grpc::ClientContext context;
  google::protobuf::Empty response;

  grpc::Status status = stub_->SetMonitorInfo(&context, info, &response);

  if (status.ok()) {
    std::cout << ">>> Pushed monitor data to " << manager_address_ << " successfully <<<" << std::endl;
    return true;
  } else {
    std::cerr << ">>> Push failed: " << status.error_message() << " <<<" << std::endl;
    return false;
  }
}

}  // namespace monitor
