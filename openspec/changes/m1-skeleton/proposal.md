# Proposal

## Why

开发文档里程碑 M1：先用 /proc 打通 Worker 采集 → 上报 → Manager 接收 → Redis 的全链路，后面的内核模块、eBPF、多网卡、调度都在这条链路上逐个替换和叠加。现有 worker/manager/proto 代码的结构、协议和存储方式与开发文档完全不同，继续沿用会拖累后续每个里程碑，因此在 M1 一并按新结构重建。

## What Changes

- **BREAKING** 删除旧的 `worker/`、`manager/`、`proto/` 代码和 `manager/sql/`，按开发文档目录重建；旧的 MonitorInfo 推送协议、9 个查询接口和旧表结构不再保留。
- 新增 `proto/monitor.proto`（Collector 服务：Report、OnTaskEvent）、`proto/task.proto`、`proto/advisor.proto`，三份协议按开发文档定义一次性落地并参与代码生成；本里程碑只实现 Collector。
- 新增 `common/`：CAS 环形缓冲、YAML 配置加载、spdlog 日志初始化。
- 新增 Worker 骨架：采集线程每秒从 /proc 读 CPU（全机与每核）、内存、磁盘利用率写入环形缓冲；上报线程每秒取最新样本调用 Report。调度延迟、网卡字段暂不填。
- 新增 Manager 骨架：Collector gRPC 服务接收上报，更新内存节点表（含最近上报时间），由存储线程写 Redis 热数据；OnTaskEvent 先返回 Ack 不处理。
- 新增顶层 CMake 构建（C++17、GoogleTest、ThreadSanitizer 选项）、`config/worker.yaml` 与 `config/manager.yaml` 样例。
- compose 加入 manager 服务；新增 `deploy/install-worker.sh` 和 `deploy/systemd/lm-worker.service`。

## Capabilities

### New Capabilities
- `metric-protocol`: Worker 与 Manager 之间的上报协议契约（样本字段语义、Report/OnTaskEvent 行为）。
- `worker-metric-collection`: Worker 每秒采集节点 CPU、内存、磁盘指标并形成样本。
- `worker-reporting`: Worker 采集与上报解耦、每秒上报最新样本、失败不重传。
- `manager-ingest`: Manager 接收上报、维护节点表与新鲜度。
- `manager-hot-storage`: Manager 把节点最新状态与每秒样本写入 Redis 热数据，并按保留期修剪。

### Modified Capabilities
（无，openspec/specs 目前为空）

## Impact

- 对应里程碑：M1。影响模块：proto、common、worker、manager、deploy；依赖 M0 的 Redis 与 Worker 环境。
- 新依赖：gRPC/Protobuf、spdlog、yaml-cpp、GoogleTest、redis-plus-plus（Manager）。
- README 的构建与运行章节需按新结构重写。
- 不在范围内：内核模块读 CPU（M2）、调度延迟（M3）、网卡链路质量与流量（M4）、评分/剔除/任务分配与 OnTaskEvent 处理（M5）、Worker 的 WorkerTask 服务（M5/M6）、MySQL 冷数据与 HTTP/WebSocket 接口（M7）、AI 与看板（M7–M9）。
