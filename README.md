# monitor_system

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![C++](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://isocpp.org/)
[![gRPC](https://img.shields.io/badge/gRPC-1.50+-green.svg)](https://grpc.io/)
[![Platform](https://img.shields.io/badge/platform-Linux-lightgrey.svg)](https://www.linux.org/)

Linux 分布式服务器性能监控系统。Worker 部署在被监控机器上，通过内核模块、eBPF 和 procfs
采集系统指标，以 Push 模式经 gRPC 上报给 Manager；Manager 负责健康评分、MySQL 持久化和查询服务。

> 本项目正在进行大规模重构与二次开发，接口和目录结构可能随时变化。

## 特性

- **低开销采集**：内核模块 + eBPF，缺少 eBPF 依赖时自动回退到 procfs
- **全面指标**：CPU、负载、软中断、内存、磁盘、网络
- **Push 架构**：Worker 定时主动推送，Manager 无需轮询
- **健康评分**：多维度加权评分，快速判断服务器状态
- **查询服务**：gRPC QueryService，支持历史、趋势、异常、排名及各类明细查询
- **持久化**：MySQL 存储历史数据

## 架构

```
┌─────────────────┐     gRPC Push      ┌─────────────────┐
│     Worker      │ ─────────────────► │     Manager     │
│  (被监控服务器)  │    MonitorInfo     │   (管理服务器)   │
│                 │   默认每 10 秒      │                 │
│  CPU / 负载     │                    │  数据接收       │
│  软中断 / 内存  │                    │  健康评分       │
│  磁盘 / 网络    │                    │  MySQL 存储     │
│                 │                    │  查询服务       │
└─────────────────┘                    └─────────────────┘
        │                                      │
        │ 内核模块 / eBPF / procfs              │ QueryService
        ▼                                      ▼
   /dev/cpu_stat_monitor                  query_client / 其他客户端
   /dev/cpu_softirq_monitor
```

## 目录结构

```
monitor_system/
├── worker/              # 采集端
│   ├── include/         # monitor/ rpc/ utils/
│   ├── src/
│   │   ├── monitor/     # 各类 Monitor 实现
│   │   ├── rpc/         # 数据推送
│   │   ├── kmod/        # 内核模块
│   │   └── ebpf/        # eBPF 程序
│   └── scripts/
├── manager/             # 管理端
│   ├── include/
│   ├── src/
│   └── sql/             # 数据库初始化脚本
├── proto/               # Protobuf / gRPC 定义
├── openspec/            # OpenSpec 规格与变更记录
└── CMakeLists.txt
```

## 环境要求

| 组件 | 版本 |
|------|------|
| 操作系统 | Linux（Ubuntu 20.04+ / CentOS 8+） |
| 编译器 | GCC 9+ 或 Clang 10+（C++17） |
| CMake | 3.10+ |
| 内核 | 5.4+（eBPF 需要 BTF 支持） |
| MySQL | 8.0+（Manager 必需） |

## 安装依赖

```bash
# Ubuntu / Debian
sudo apt update
sudo apt install -y \
    build-essential cmake \
    libprotobuf-dev protobuf-compiler \
    libgrpc++-dev protobuf-compiler-grpc \
    linux-headers-$(uname -r) \
    mysql-server libmysqlclient-dev \
    clang llvm libbpf-dev libelf-dev zlib1g-dev bpftool
```

```bash
# CentOS / RHEL
sudo yum install -y \
    gcc-c++ cmake \
    protobuf-devel protobuf-compiler \
    grpc-devel grpc-plugins \
    kernel-devel \
    mysql-server mysql-devel \
    clang llvm libbpf-devel elfutils-libelf-devel zlib-devel bpftool
```

eBPF 程序默认从 `/usr/include/vmlinux.h` 读取内核类型定义，可用以下命令生成：

```bash
bpftool btf dump file /sys/kernel/btf/vmlinux format c | sudo tee /usr/include/vmlinux.h > /dev/null
```

## 数据库

```sql
CREATE DATABASE monitor_db;
CREATE USER 'monitor'@'localhost' IDENTIFIED BY 'monitor123';
GRANT ALL PRIVILEGES ON monitor_db.* TO 'monitor'@'localhost';
FLUSH PRIVILEGES;
```

```bash
mysql -u monitor -pmonitor123 monitor_db < manager/sql/init_server_performance.sql
```

数据库连接信息目前写在 `manager/src/main.cpp` 和 `manager/src/host_manager.cpp` 中，按需修改。

| 表名 | 说明 |
|------|------|
| `server_performance` | 性能汇总 |
| `server_net_detail` | 网络接口明细 |
| `server_disk_detail` | 磁盘设备明细 |
| `server_mem_detail` | 内存明细 |
| `server_softirq_detail` | 软中断明细 |

## 编译

```bash
git clone git@github.com:anwalld/monitor_system.git
cd monitor_system
cmake -S . -B build
cmake --build build -j$(nproc)
```

CMake 选项：

| 选项 | 默认 | 说明 |
|------|------|------|
| `BUILD_MANAGER` | `ON` | 编译 Manager（需要 MySQL） |
| `ENABLE_EBPF` | `ON` | 启用 eBPF 网络采集，依赖缺失时自动回退 procfs |

内核模块单独编译：

```bash
make -C worker/src/kmod
```

## 运行

### 1. 启动 Manager

```bash
./build/manager/manager
```

默认监听 `0.0.0.0:50051`。

### 2. 加载内核模块

```bash
sudo insmod worker/src/kmod/cpu_stat_collector.ko
sudo insmod worker/src/kmod/softirq_collector.ko
ls /dev/cpu_stat_monitor /dev/cpu_softirq_monitor
```

### 3. 启动 Worker

```bash
# 参数：Manager 地址 [推送间隔秒数，默认 10]
sudo ./build/worker/worker 192.168.1.100:50051
```

加载 eBPF 程序需要 root 权限。

### 4. 查询历史数据

```bash
# 参数：Manager 地址、主机标识、起始时间、结束时间、[页码=1]、[每页条数=100]
./build/manager/query_client 127.0.0.1:50051 server1_192.168.1.101 \
  '2026-09-17T00:00:00+08:00' '2026-09-17T23:59:59+08:00' 1 20
```

- 主机标识为 Manager 日志中的 `hostname_ip`
- 时间使用带时区的 RFC3339 格式（`Z` 或 `+08:00`），按秒查询
- 超时 10 秒；退出码：成功 0，RPC 失败 1，参数错误 2
- `./build/manager/query_client --help` 查看说明

### 5. 停止与卸载

```bash
sudo rmmod softirq_collector
sudo rmmod cpu_stat_collector
```

## 监控指标

| 监控项 | 数据来源 | 内容 |
|--------|----------|------|
| CPU 状态 | 内核模块 / procfs | 各核使用率、用户态/内核态/空闲占比 |
| CPU 负载 | `/proc/loadavg` | 1/5/15 分钟平均负载 |
| 软中断 | 内核模块 / procfs | 各核软中断统计 |
| 内存 | `/proc/meminfo` | 总量、可用、缓存、Swap 等 |
| 磁盘 | `/proc/diskstats` | 读写速率、IOPS、延迟、利用率 |
| 网络 | eBPF / procfs | 收发速率、包数、错误与丢包 |

## 查询接口

| 接口 | 功能 |
|------|------|
| `QueryPerformance` | 时间段性能数据 |
| `QueryTrend` | 变化率趋势 |
| `QueryAnomaly` | 异常数据检测 |
| `QueryScoreRank` | 评分排名 |
| `QueryLatestScore` | 最新评分 |
| `QueryNetDetail` | 网络明细 |
| `QueryDiskDetail` | 磁盘明细 |
| `QueryMemDetail` | 内存明细 |
| `QuerySoftIrqDetail` | 软中断明细 |

## 健康评分

```
Score = CPU × 35% + Mem × 30% + Load × 15% + Disk × 15% + Net × 5%

CPU  = 1 - cpu_percent / 100
Mem  = 1 - mem_used_percent / 100
Load = 1 - load_avg_1 / (cpu_cores × 1.5)
Disk = 1 - disk_util_percent / 100
Net  = 1 - bandwidth_usage / max_bandwidth
```

## 关键参数

| 参数 | 默认值 | 说明 |
|------|--------|------|
| 推送间隔 | 10 秒 | Worker 推送周期 |
| 离线阈值 | 60 秒 | 超时无数据视为离线 |
| gRPC 端口 | 50051 | Manager 监听端口 |
| 主机标识 | `hostname_ip` | 如 `web-server_10.0.0.5` |

## 开发流程

项目使用 [OpenSpec](https://github.com/Fission-AI/OpenSpec) 管理需求与变更，项目约定见
[`openspec/config.yaml`](openspec/config.yaml)。

```bash
openspec list            # 查看进行中的变更
openspec validate --all  # 校验规格
```

## 许可证

[MIT](LICENSE)
