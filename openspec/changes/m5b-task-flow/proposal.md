# Proposal

## Why

对应开发文档里程碑 M5 的后半部分。m5a 让 Manager 会打分、会按权重选节点，但它不知道选出去的任务后来怎么样了：一台节点刚被派了 50 个任务、还没开始跑，分数照样很高，下一个任务还会派给它；Worker 收不到或跑不完任务，也不会影响它被选中的概率。另外 Worker 还不能执行任务，“本地负载出现时任务自动转移”这条验收也就没法做。

本变更补上任务的完整流程和在途账，让 M5 达到开发文档的验收标准：单元测试全过；本地负载出现时任务自动转移。

## What Changes

- **Manager 在途任务账**（`manager/inflight/`）：
  - 每个任务四步：`Allocate` 分配节点 → 客户端 `MarkSent` → Worker 上报 RECEIVED → Worker 上报 DONE 或 FAILED。
  - 两条超时链表（`std::list` 按加入顺序 + `unordered_map<task_id, iterator>`）：确认链表 30 秒、执行链表 300 秒，每 100 毫秒只看两条链表的头。
  - 确认超时：总数和在途一起减掉；客户端已经 `MarkSent` 的计一次“未确认”，没有 `MarkSent` 的只作废。执行超时：在途减一，超时计数 T 加一。迟到的完成事件查不到 task_id，只计数、不动账。
  - 选择因子：`f_inflight = 1 / (1 + max(0, r − 1))`，r 取在途字节数和在途任务数各自除以基线后的较大值，基线是自身在途量的指数平滑（时间常数 60 秒，每秒更新，带下限）；`f_timeout = exp(−λ·T)`，T 按半衰期 10 分钟衰减。
- **Manager 调度主循环**：新线程，每 100 毫秒处理超时链表，每秒更新基线、衰减 T。
- **`Scheduler.MarkSent`** 实现；`Collector.OnTaskEvent` 接到在途账上销账。
- **Worker 任务服务**（`worker/task/`）：在 50052 端口提供 `WorkerTask`：
  - `Submit`：先异步上报 RECEIVED，再用 `posix_spawn` 执行命令，超时杀掉整个进程组；结束后上报 DONE（退出码 0）或 FAILED，返回退出码、输出末尾 4 KiB 和执行耗时。
  - `Probe`：只回 seq（M6 的低频探测要用，实现很小，一起做）。`Rescue` 返回 `UNIMPLEMENTED`（M6）。
  - 安全：默认关闭；命令必须在 `task.allowed_commands` 白名单里；可选静态 token；子进程不继承 Worker 的能力（启动时清空 ambient capability）；并发上限。
  - 任务事件由单独的发送线程异步上报，每个事件最多尝试 3 次。
- **压测客户端**：`experiments/lm-bench` 增加 `submit` 模式，按固定速率、固定随机种子执行“Allocate → MarkSent → 直连 Worker Submit”，每秒打印各节点的任务占比，结束时打印完成时间 P50、P99、最大值和失败数。
- **多节点验收环境**：现在只有 1 台 Worker 虚拟机。同一台虚拟机上起第二个 Worker 没有意义（本地负载对两者一样），所以第二个节点用 Mac 上 Docker Desktop 里的 Worker 容器（仅用于测试，见 design）。

## Capabilities

### New Capabilities
- `manager-inflight`：在途任务账、四步流程、两条超时链表、确认与执行超时、迟到事件、`MarkSent`、`OnTaskEvent` 销账、f_inflight 与 f_timeout、调度主循环。
- `worker-task-exec`：`WorkerTask.Submit` 的执行、超时、输出、事件上报，`Probe`，白名单、token、能力隔离、并发上限与配置。
- `bench-client`：`lm-bench submit` 的参数、负载模型、输出与统计。

### Modified Capabilities
- `manager-allocation`（m5a 新增，未归档）：选择权重里的 f_inflight、f_timeout 不再恒为 1；`MarkSent` 不再返回 `UNIMPLEMENTED`。以 ADDED 增量写新要求，归档时与 m5a 合并。

## Impact

- 对应里程碑：M5（后半）。影响模块：manager（新增 `inflight/`，改 `api/`、`ingest/`、`main.cpp`）、worker（新增 `task/`，改 `main.cpp`）、experiments、config、deploy（systemd 单元不变，配置模板增加 `task` 段）、tests。
- proto 不改：`TaskEvent`、`WorkerTask`、`MarkSent` 在 M1 都已定义。
- Redis：`node:{id}` 增加 `inflight_tasks`、`inflight_bytes`、`f_inflight`、`f_timeout`、`timeout_count`、`unconfirmed` 6 个字段，仍在同一个 HSET 里，随上报每秒写一次。MySQL 的 `task` 表留给 M7。
- 调度锁：每次 Allocate、MarkSent、事件多一次 O(1) 链表操作；主循环每 100 毫秒持锁一次处理链表头，每秒持锁遍历 20 台节点更新基线。
- Worker 开销：任务服务空闲时只有 gRPC 线程池；执行任务的开销算在任务自己身上。
- 安全：Worker 开始接受远程命令。默认 `task.enabled: false`，必须显式开启并配置白名单。
- 不在范围内：
  - 节点状态机、慢启动（f_slow）、急救、低频探测的调度和灰色故障剔除（M6）。本变更只提供 `Probe` 接口和“未确认”计数。
  - `task` 表写 MySQL、看板展示（M7）。
  - 对照实验（M10）。lm-bench 只做到 M5 验收需要的程度。
  - 认证以外的多租户隔离、任务排队（并发超限直接拒绝）。
