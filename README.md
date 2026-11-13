# linux-monitor

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![C++](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://isocpp.org/)
[![Platform](https://img.shields.io/badge/platform-Linux-lightgrey.svg)](https://www.linux.org/)

Linux 服务器性能监控与调度系统。Worker 采集节点指标，Manager 给每台节点打分，把每个任务派给此刻最合适的节点。AI 服务负责异常诊断、运维问答、负载预测和日报，Web 看板实时展示。

> 正在按里程碑重构，目前完成到 M1：/proc 采集 → 上报 → Manager → Redis 的链路已打通。进度见下文“里程碑”。

## 目标

- **派给最闲的节点**：任务派给当前最闲的节点。别人直接登录节点跑的负载，Manager 也看得到。
- **故障隔离**：故障节点 3 秒内隔离，恢复后自动慢启动、重新接任务。
- **低开销采集**：CPU 读取耗时比 `/proc/stat` 低 85% 以上，撑得住 1ms 级的急救探测。
- **AI 只给建议**：AI 服务整个挂掉，调度照常运行。

## 架构

```
            gRPC Report（每秒）              旁路（挂掉不影响调度）
 ┌────────┐ ───────────────────► ┌─────────┐ ──► Redis（热数据） ──► AI 服务
 │ Worker │                      │ Manager │ ──► MySQL（5 分钟聚合）
 │  ×N    │ ◄─── Probe/Rescue ── │         │ ──► WebSocket ──► 看板
 └────────┘                      └─────────┘
     ▲  任务直连 Submit                ▲ Allocate
     └──────────── 客户端 ─────────────┘
```

调度链路上只有 Worker 和 Manager 两方。客户端从 Manager 拿到 Worker 地址后，直连 Worker 提交任务，任务数据不经过 Manager。

## 目录

```
proto/        monitor.proto（上报）  task.proto（任务/探测）  advisor.proto（AI 预测）
common/       环形缓冲、配置、日志
worker/       collector/（CPU、内存、磁盘）  reporter/（每秒上报）  main.cpp
manager/      ingest/（接收上报、节点表）  storage/（Redis 热数据）  main.cpp
tests/        GoogleTest 单元测试与 Redis 集成测试
config/       worker.yaml、manager.yaml 样例
deploy/       docker-compose、MySQL 表结构、策略路由与自检脚本、systemd、安装脚本
openspec/     需求规格与按里程碑划分的变更
```

后续里程碑会加入 `kmod/`、`bpf/`、`ai/`、`web/`、`experiments/`。

## 构建

依赖：

- CMake ≥ 3.20、C++17 编译器
- gRPC、Protobuf、spdlog、yaml-cpp
- hiredis（Manager 需要）

redis-plus-plus 和 GoogleTest 找不到时，CMake 会自动下载源码构建。

### Ubuntu 24.04（Worker 与 Manager 的目标环境）

```bash
sudo apt install -y build-essential cmake ninja-build pkg-config \
    libgrpc++-dev protobuf-compiler-grpc libprotobuf-dev protobuf-compiler \
    libspdlog-dev libyaml-cpp-dev libhiredis-dev
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

### macOS（开发机）

```bash
brew install cmake ninja grpc protobuf spdlog yaml-cpp hiredis
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH="$(brew --prefix)"
cmake --build build && ctest --test-dir build
```

macOS 上能编译、能跑单元测试，但 Worker 依赖 `/proc`，只能在 Linux 上运行。

### 在 macOS 上用容器跑 Linux 构建与测试

```bash
docker build -f deploy/docker/builder.Dockerfile -t lm-builder .
docker run --rm -v "$PWD":/src -w /src lm-builder bash -c \
  'cmake -S . -B build-linux -G Ninja && cmake --build build-linux && ctest --test-dir build-linux'
```

### CMake 选项

| 选项 | 默认 | 说明 |
|------|------|------|
| `LM_BUILD_MANAGER` | ON | 编译 Manager |
| `LM_BUILD_TESTS` | ON | 编译单元测试 |
| `LM_ENABLE_TSAN` | OFF | 开启 ThreadSanitizer。用到 gRPC 的测试在该模式下不注册 |

Redis 集成测试需要设置环境变量 `LM_TEST_REDIS_URI=tcp://host:6379`（有密码时再设 `LM_TEST_REDIS_PASSWORD`），否则跳过。

## 运行

搭建环境的详细步骤见 [deploy/README.md](deploy/README.md)。

### Manager 机

```bash
cd deploy
cp .env.example .env      # 填写三个密码
docker compose up -d --build --wait
```

compose 会启动 Redis、MySQL 和 Manager。Manager 用 host 网络，gRPC 监听 50051 端口。

### Worker 机

第一步，每台 Worker 机先做两件事：

1. 运行 `deploy/check-worker-host.sh` 自检。
2. 用 `deploy/policy-route.sh` 配好多网卡策略路由。

第二步，在一台与 Worker 同架构的 Linux 上构建出 `build-linux/worker/worker`，然后批量安装：

```bash
deploy/install-worker.sh -m <manager_ip>:50051 -u ubuntu worker1 worker2 worker3
```

安装脚本会做这些事：

- 装好运行时库
- 创建系统用户 `lm`
- 写入 `/etc/lm/worker.yaml`，其中 `node_id` 留空，表示使用主机名
- 用 systemd 启动 `lm-worker`

### 单机调试

```bash
./build/worker/worker -c config/worker.yaml --dry-run    # 不上报，每秒把样本以 JSON 打印出来
LM_REDIS_PASSWORD=... ./build/manager/manager -c config/manager.yaml
```

### 查看数据

```bash
redis-cli HGETALL node:<node_id>                         # 最新指标
redis-cli ZRANGE node:<node_id>:ts -5 -1                  # 最近 5 秒样本
redis-cli XREVRANGE stream:metrics + - COUNT 1            # 最近一秒所有节点
```

## 里程碑

| 阶段 | 内容 | 状态 |
|------|------|------|
| M0 环境 | 双网卡策略路由、主机自检、Redis/MySQL 编排 | 脚本完成，待真实 Worker 验收 |
| M1 骨架 | proto、/proc 采集、环形缓冲、上报、Manager 写 Redis | 代码完成，待真实 Worker 验收 |
| M2 内核模块 | cpustat 共享页 + seqlock，替换 /proc 读 CPU | |
| M3 eBPF | 调度延迟直方图 | |
| M4 多网卡 | ICMP 探测、TCP 重传率、流量 | |
| M5 调度 | 评分、剔除、概率选择、在途账、任务流程 | |
| M6 节点状态机 | 熔断、急救、慢启动、退避 | |
| M7 存储与看板 | MySQL 冷数据、REST/WebSocket、Vue 看板 | |
| M8–M9 AI | 异常诊断、运维问答、负载预测、日报 | |
| M10 对照实验 | 与轮询、加权轮询对比 | |

每个里程碑对应 `openspec/changes/` 下的一个变更，开发流程见 [OpenSpec](https://github.com/Fission-AI/OpenSpec)。

## 许可证

[MIT](LICENSE)
