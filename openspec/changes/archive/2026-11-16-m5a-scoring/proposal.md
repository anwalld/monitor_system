# Proposal

## Why

对应开发文档里程碑 M5 的前半部分。M1–M4 已经把 CPU、调度延迟、内存、磁盘、每块网卡的链路质量每秒送到 Manager，但 Manager 只是把它们写进 Redis，还不会用它们做决定。M5 的目标是“把任务派给此刻最合适的节点”。按交接文档的建议，M5 拆成两个变更：

- `m5a-scoring`（本变更）：评分、剔除、按权重概率选择，以及 `Scheduler.Allocate`。
- `m5b-task-flow`：在途任务账、`MarkSent`、Worker 的 `WorkerTask.Submit`、任务事件销账、压测客户端。

本变更结束时，客户端调用 `Allocate` 就能拿到一台按分数加权随机选出的 Worker 地址；剔除的、失联的节点不会被选中。

## What Changes

- Manager 新增 `manager/scoring/`：
  - 归一化：每个指标有好点、坏点，`s = clamp((坏点 − x) / (坏点 − 好点), 0, 1)`；调度延迟 P99 先取对数再映射。
  - 网络分：每块网卡取 RTT、超时率、重传率三项分数的最小值，节点取最好的那块网卡。
  - 加权：`score = Σ wₖ·sₖ / Σ wₖ`。没有上报调度延迟或没有网卡数据的项按 0.3 计，每个节点首次出现时打一条提示日志。
  - 关键指标剔除：CPU、调度延迟 P99、可用内存、网络（所有网卡超时率）越过剔除线连续 3 次才剔除，回到恢复线以内连续 5 次才恢复；越过剔除线 10 倍立即剔除。
  - 选择：候选节点是未剔除、3 秒内有上报的节点，权重 `w = score × f_inflight × f_timeout × f_slow × f_pred`，按前缀和加均匀随机数加二分查找抽样。本变更中后四个因子恒为 1，接口留好。
- `NodeTable` 在每次接受上报时，于同一把锁内重算分数、更新剔除状态（纯内存，微秒级）。
- Manager 新增 `Scheduler` gRPC 服务（与 `Collector` 同一个端口 50051）：
  - `Allocate`：返回 `task_id` 和 `worker_addr`；没有候选或权重全为 0 时返回 `RESOURCE_EXHAUSTED`。
  - `MarkSent`：本变更返回 `UNIMPLEMENTED`，m5b 实现。
- `node:{id}` Hash 增加 `score`、`s_cpu`、`s_runq`、`s_mem`、`s_net`、`best_nic`、`evicted`、`evict_reason` 字段。
- `config/manager.yaml` 增加 `scoring` 段：各指标好点、坏点、剔除线、恢复线、权重，缺失项分数、失联时限、连续次数。缺省值取开发文档“默认参数”表。

## Capabilities

### New Capabilities
- `manager-scoring`：归一化与对数映射、网络分、加权评分、缺失项处理、关键指标剔除与恢复（迟滞、去抖、10 倍立即剔除）、失联判定。
- `manager-allocation`：候选集合、权重与因子、加权随机抽样、`Scheduler.Allocate` 的请求、返回与错误码、`task_id` 格式。

### Modified Capabilities
- `manager-hot-storage`：`node:{id}` 增加分数明细与剔除状态字段。M1、M3 都还没归档，本变更以 ADDED 增量写新增的要求，不改原有要求。

## Impact

- 对应里程碑：M5（前半）。影响模块：manager（新增 `manager/scoring/`、`manager/api/`，修改 `ingest/`、`storage/`、`main.cpp`）、config、tests。
- proto 不改：`Scheduler`、`AllocateReq`、`AllocateResp` 在 M1 已定义。
- Redis：`node:{id}` 每条上报多写 8 个字段（约 120 字节），仍在同一个 pipeline 里，不增加往返。旧字段不变，旧读者不受影响。MySQL 不涉及。
- 调度锁：每条上报多一次评分（约 20 次浮点运算、最多 4 块网卡），`Allocate` 在锁内遍历全部节点（上限 20 台），都在微秒级。
- 不在范围内：
  - 在途任务账、`MarkSent`、f_inflight、f_timeout、确认与执行超时（m5b）。
  - Worker 的 `WorkerTask` 服务、压测客户端、“本地负载出现时任务自动转移”的验收（m5b）。
  - 节点状态机、慢启动 f_slow、急救、低频探测、灰色故障剔除、“剔除 → 慢启动”的转移（M6）。本变更里剔除只是一个布尔状态，失联的节点重新上报后立即回到候选集合。
  - AI 预测因子 f_pred（M9）。
  - 分数推给看板（M7）。
