# Design

## Context

见 proposal.md。现有代码是旧架构（MonitorInfo 推送、Monitor 类继承体系、manager 直写 MySQL），与开发文档的协议、目录、线程模型都对不上。M1 在新目录下从零搭骨架，旧代码整体删除（git 历史可查）。开发机 macOS 能编译用户态 C++ 与跑单元测试，/proc 采集只能在 Linux 上真正验证。

## Goals / Non-Goals

**Goals:**
- 目录、CMake 目标、线程划分一次定型，M2–M7 只往里加模块，不再挪结构。
- Worker 的 CPU 读取做成接口，M2 用共享页实现替换 /proc 实现时上层不改。
- Manager 的“调度状态 + 一把锁 + 存储线程”模式在 M1 就成形。

**Non-Goals:**
- 不做评分、剔除、任务分配（M5）；不做 MySQL 写入与 HTTP 接口（M7）。
- 不实现 WorkerTask、Scheduler、Advisor 服务，只生成代码。

## Decisions

### 1. 目录与 CMake 目标
```
proto/            → 静态库 lm_proto（三份 proto 全部生成）
common/           → 静态库 lm_common（ring.h、config、log）
worker/collector/ → lm_worker_collector（CpuSource 接口 + ProcCpuSource、mem、disk）
worker/reporter/  → lm_worker_reporter
worker/main.cpp   → 可执行 worker
manager/ingest/   → lm_manager_ingest（Collector 服务、NodeTable）
manager/storage/  → lm_manager_storage（RedisWriter）
manager/main.cpp  → 可执行 manager
tests/            → 每个库一个 gtest 目标，ctest 统一跑
```
`worker/netprobe`、`worker/task`、`manager/{scoring,inflight,breaker,api}` 在对应里程碑再建，M1 不放空目录。

### 2. 依赖获取：优先系统包，GoogleTest 用 FetchContent
gRPC、Protobuf、spdlog、yaml-cpp、redis-plus-plus 用 `find_package`（Ubuntu 24.04 apt 均可装，macOS 用 Homebrew）；GoogleTest 用 FetchContent 固定版本，避免各机器版本不一。提供 `LM_ENABLE_TSAN`、`LM_BUILD_MANAGER`、`LM_BUILD_TESTS` 三个选项。
- 备选：全部 FetchContent 或 vcpkg。gRPC 源码构建太慢，不采用。

### 3. 环形缓冲按开发文档实现，模板化槽位类型
`lm::Ring<T, N=8>`，槽 `alignas(64)`，状态 EMPTY/WRITING/READY/READING，`publish` 跳过 READING 槽、覆盖最旧 READY，`take_latest` CAS 抢 READY→READING 后拷出立即释放。放在 common/ 以便 M2 的单测和 Manager 侧复用。

### 4. CPU 来源抽象
`CpuSource::read(CpuSnapshot&)` 返回每核累计 `idle+iowait` 与时间戳。M1 实现 `ProcCpuSource`：常开 `/proc/stat` fd，每次 `pread` 并只解析 `cpuN` 行，时间戳用 Δtotal（jiffies）。忙碌度计算放在通用的 `CpuBusyCalculator`，M2 换成共享页实现时只替换来源（时间戳改为 stamp_us）。

### 5. Worker 线程模型
- 采集线程：`clock_nanosleep(TIMER_ABSTIME)` 按绝对时间对齐每秒，避免漂移；采完 `ring.publish`。
- 上报线程：每秒 `take_latest`，`seq` 不大于上次发送的跳过；同步 stub 调用、500ms deadline。
- 主线程：信号处理，SIGINT/SIGTERM 时停两个线程并等待退出。

### 6. Manager 节点表与存储线程
- `NodeTable`：`std::unordered_map<std::string, NodeState>` + 一把 `std::mutex`。Report 处理在锁内完成登记、seq 检查、更新，锁外把 `StorageOp` 推入存储队列。
- 存储队列：有界 MPSC 队列（容量 4096），满时丢弃最旧并计数，保证 Redis 宕机时内存不涨。
- 存储线程：每 100ms 或攒满 256 条刷一次，用 redis-plus-plus pipeline 一次往返完成 HSET/ZADD；每秒一次 XADD `stream:metrics`；每分钟对每个节点 ZREMRANGEBYSCORE。
- 节点地址：协议里没有地址字段，取 gRPC `context->peer()` 的 IP，加配置的 `worker_port`（默认 50052）。
- 备选：在 Sample 里加 `addr` 字段。会让 Worker 自己判断用哪个地址，多网卡下容易报错地址；对端 IP 就是 Manager 能连回去的地址，更可靠。

### 7. 节点标识
`worker.yaml` 的 `node_id`，缺省为主机名。Manager 不校验格式，只要求非空且不超过 64 字节（与 MySQL `node.node_id VARCHAR(64)` 一致），超长返回 INVALID_ARGUMENT。

### 8. 配置
`config/worker.yaml`：`node_id`、`manager_addr`、`collect_interval_ms`、`report_deadline_ms`、`log_level`。
`config/manager.yaml`：`listen`（0.0.0.0:50051）、`worker_port`、`redis.uri`、`redis.ts_retention_s`（3600）、`log_level`。Redis 密码从环境变量读取。

### 9. 实现中补充的决定（2026-11-13）
- **环形缓冲只返回更新的样本**：`take_latest` 记住上次取到的 seq，只返回比它新的；消费者比生产者快时不会读回旧样本，读到的 seq 严格递增。
- **上报错开半个周期**：采集与上报同为 1 秒且同时启动时节拍重合，抖动导致漏报（实测 12 条报 8 条）。Reporter 首拍延迟 interval/2（`Options::phase`），改后每拍恰好一条新样本。
- **Worker 重连退避上限 1 秒**：gRPC 默认退避会增长到数十秒，实测 Manager 重启后 26 秒才恢复上报，会超过 3 秒失联阈值。通道参数设 initial/min 500ms、max 1000ms，实测恢复 4.8 秒（含容器启动）。
- **redis-plus-plus 从源码构建**：Ubuntu 24.04 与 Homebrew 均无此包，CMake 用 FetchContent 拉取 1.3.13，并补生成头文件路径与 hiredis 链接。
- **TSan 下不跑 gRPC 测试**：系统 gRPC/absl 未用 TSan 编译，误报多；`test_worker_rpc`、`test_manager_rpc` 在 `LM_ENABLE_TSAN=ON` 时不注册，其余测试都在 TSan 下运行。
- **Manager 的 Redis 地址可由 `LM_REDIS_URI` 覆盖**：compose 用它指向 `127.0.0.1:${REDIS_PORT}`。
- **磁盘利用率验收改为对齐内核**：虚拟 NVMe 上内核 io_ticks 本身只增长 36–65%，fio 压满也到不了 0.8。磁盘利用率只展示、不进评分，所以验收改为“压测时 ≥ 0.3，且与同期 io_ticks 算出的 %util 误差 ≤ 0.05”（用户 2026-11-13 确认）。实测最大误差 0.006。

## Risks / Trade-offs

- [macOS 上没有 /proc，Worker 无法在开发机运行] → collector 的解析函数接受文本输入，单测用 fixture 文件覆盖；Worker 二进制在 macOS 上仍能编译，运行时检测到没有 /proc 直接报错退出。
- [peer IP 在 NAT 或 Docker 网络后不是 Worker 真实地址] → Manager 容器用 host 网络模式；在 deploy/README 中说明。
- [Redis 写入每节点每秒两条命令，20 节点约 40 次/秒] → pipeline 批量提交，负载可忽略。
- [删除旧代码后 README 与构建说明短期不一致] → 同一组任务里重写 README。

## Migration Plan

旧代码直接删除，不做兼容；旧 MySQL 表不再使用（M0 的 init.sql 是新库）。回滚：git revert 本 change 的提交。

## Open Questions

- Worker 的 ICMP/网卡字段在 M1 是否需要占位输出：当前决定不填 `nics`，M4 再填，不影响 M1 验收。
