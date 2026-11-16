# Design

## Context

- m5a 已有 `NodeTable`（一把锁保护）、`select()` 里留好了 `Factors`、`SchedulerService::Allocate` 生成 `task_id`。
- `Collector.OnTaskEvent` 在 M1 只确认收到；`MarkSent` 在 m5a 返回 `UNIMPLEMENTED`。
- Worker 只有采集、探测、上报三个线程，没有 gRPC 服务端。systemd 单元给 Worker 进程授予了 ambient capability `CAP_NET_RAW CAP_BPF CAP_PERFMON`，如果直接 `posix_spawn`，子进程会继承这些能力。
- 虚拟机只有 1 台。

## Goals / Non-Goals

**Goals**
- 在途账严格按开发文档的四步和两条链表实现，用模拟时钟单测覆盖“未确认的作废、确认后超时才计数、迟到的完成不重复计”。
- Worker 能安全地执行白名单里的命令，事件尽量不丢。
- 在两台 Worker 上演示：一台出现本地负载后，它的任务占比明显下降，负载消失后回升。

**Non-Goals**
- 状态机、慢启动、急救（M6）；`task` 表（M7）；对照实验（M10）。

## Decisions

### 1. 目录与模块

```
manager/inflight/
  inflight_ledger.{h,cpp}  InflightLedger：任务表、两条超时链表、每节点计数、基线、T；纯内存，时钟由调用方传入
manager/api/scheduler_service.*   Allocate 记账、MarkSent
manager/ingest/collector_service.*  OnTaskEvent 销账
manager/scheduler_loop.{h,cpp}    调度主循环线程（100 ms）
worker/task/
  task_runner.{h,cpp}      posix_spawn、进程组、超时、输出末尾；纯 POSIX，各平台都编
  event_sender.{h,cpp}     任务事件队列 + 发送线程（重试 3 次）
  task_service.{h,cpp}     WorkerTask gRPC 服务：白名单、token、并发上限
experiments/lm_bench.cpp   allocate、submit
```

### 2. 在途账放在 NodeTable 里

- 在途账和节点表必须在同一把锁里：`select()` 算权重要读每个节点的在途量和 T，分开两把锁就得在 `select` 里嵌套加锁或者拷一份快照。所以 `InflightLedger` 作为 `NodeTable` 的成员，由 `NodeTable` 的锁保护；`InflightLedger` 本身不加锁，便于单测。
- `NodeTable` 新增的接口都是“加锁 → 调 ledger → 解锁”：`allocate()`（select + 记账一次完成，避免选中和记账之间节点状态变了）、`mark_sent()`、`on_event()`、`tick(now)`、`tick_second(now)`。

### 3. 任务状态与链表

| 状态 | 在哪条链表 | 进入时机 | 截止时间 |
|---|---|---|---|
| ALLOCATED | 确认链表 | Allocate | 分配时刻 + 30 s |
| SENT | 确认链表（移到队尾） | MarkSent | MarkSent 时刻 + 30 s |
| RUNNING | 执行链表 | RECEIVED | 收到时刻 + 300 s |

- 同一条链表里的截止时间单调不减（时限统一、移到队尾时重新计时），所以 `tick` 只需从头弹出过期项。
- `MarkSent` 只对 ALLOCATED 有效；对 SENT、RUNNING 重复调用返回 OK 且不改状态（幂等）；task_id 不存在返回 `NOT_FOUND`。
- RECEIVED：ALLOCATED 或 SENT → RUNNING（客户端可能没调 MarkSent 就直接提交，或 MarkSent 晚于 RECEIVED 到达）。RUNNING 时重复的 RECEIVED 忽略。
- DONE / FAILED：任意状态 → 删除，在途任务数减一、在途字节数减去 `input_bytes`（RECEIVED 丢了也能正确销账）。
- 迟到的事件（查不到 task_id）：计入 `late_events`，打 debug 日志，不动任何计数。M7 会用它更新 `task` 表。
- 确认超时：
  - SENT：节点“未确认”计数加一（M6 的灰色故障判断要用），总数和在途一起减。
  - ALLOCATED：客户端拿到地址却没发，不怪 Worker，只作废，总数和在途一起减。
  - 开发文档只说“确认超时只计入未确认统计”，没区分是否 MarkSent。`MarkSent` 存在的意义就是区分“客户端没发”和“Worker 没收到”，所以这样处理。
- 执行超时：在途减一，T 加一。“总数”是分配给该节点、还没被作废的任务数，执行超时的任务仍算在总数里。
- 每节点计数：`assigned_total`（累计分配，不减；给看板）、`total`（文档里的总数）、`inflight_tasks`、`inflight_bytes`、`unconfirmed`、`exec_timeouts`、`T`。
- 节点消失（M6 才会删除节点）：本变更里节点表只增不删，不用处理。

### 4. 选择因子

- `f_inflight = 1 / (1 + max(0, r − 1))`，`r = max(inflight_tasks / base_tasks, inflight_bytes / base_bytes)`。
- 基线：每秒 `base ← base + α·(x − base)`，`α = 1 − exp(−1 s / 60 s)`，x 是当时的在途量；参与计算时取 `max(base, floor)`，下限 `base_tasks_floor = 2`、`base_bytes_floor = 1 MiB`。新节点基线从 0 开始，靠下限兜底。
  - 含义：节点手里的在途量突然超过自己平时水平（和下限）时降权；平稳时 r ≈ 1，f_inflight ≈ 1，不影响按分数分配。
- `f_timeout = exp(−λ·T)`，T 每秒乘以 `0.5^(1 s / 600 s)`（半衰期 10 分钟）。开发文档没给 λ，取 `λ = 0.5`：一次执行超时后 f_timeout ≈ 0.61，三次 ≈ 0.22，10 分钟后减半。可配置。
- 慢启动期间暂停在途系列的二次调整是 M6 的事，本变更不处理。

### 5. 调度主循环

- `SchedulerLoop` 线程：每 100 ms 调一次 `NodeTable::tick(now)`（处理两条链表头），每满 1 s 调一次 `tick_second(now)`（基线、T 衰减）。用单调时钟，sleep 到下一个整 100 ms，迟到不补拍。
- 超时日志：锁内只收集过期任务的 task_id 和节点，锁外打日志（每秒最多 10 条，其余合并成计数）。
- M6 的状态机、低频探测也挂在这个循环上。

### 6. Allocate、MarkSent、OnTaskEvent

- `Allocate`：`NodeTable::allocate(now, u, task_id, input_bytes)` 在一次加锁内选节点并记账。`task_id` 在锁外先生成（需要的话浪费一个序号）。
- `MarkSent`：task_id 为空 `INVALID_ARGUMENT`，不存在 `NOT_FOUND`，其余 OK。
- `OnTaskEvent`：`KIND_UNSPECIFIED` 仍返回 `INVALID_ARGUMENT`；task_id 为空 `INVALID_ARGUMENT`；迟到事件返回 OK（Worker 不需要知道）。
- 汇总日志每 10 秒增加：在途总数、确认超时（作废 / 未确认）、执行超时、迟到事件。

### 7. 写入 Redis

`AcceptedSample` 增加在途快照，存储线程在 `node:{id}` 的 HSET 里多写：`inflight_tasks`、`inflight_bytes`、`f_inflight`、`f_timeout`（4 位小数）、`timeout_count`（T，4 位小数）、`unconfirmed`（累计）。快照在 `apply()` 锁内拷出，随上报每秒更新一次，不额外写 Redis。

### 8. Worker 任务服务

**线程**：gRPC 同步服务端，线程池最多 `task.max_concurrent + 4` 个（`ResourceQuota::SetMaxThreads`），Submit 在 gRPC 线程里阻塞执行。另有一个事件发送线程。

**配置**（`worker.yaml`）：

```yaml
task:
  enabled: false              # 默认关闭
  listen: 0.0.0.0:50052
  allowed_commands: []        # 绝对路径，完全匹配；为空时拒绝所有 Submit
  token: ""                   # 非空时 Submit 要求 metadata authorization: Bearer <token>；也可用环境变量 LM_TASK_TOKEN
  max_concurrent: 0           # 0 表示 2 × 核数
  default_timeout_s: 300      # TaskReq.timeout_s 为 0 时使用
  max_timeout_s: 3600
  output_tail_bytes: 4096
```

**Submit 流程**：
1. 校验：task_id 非空且 ≤ 128 字节，否则 `INVALID_ARGUMENT`；token 不对 `UNAUTHENTICATED`；命令不在白名单 `PERMISSION_DENIED`；`timeout_s` 超过上限 `INVALID_ARGUMENT`。这些情况不上报事件：请求本身无效，Manager 那边会按确认超时作废（客户端调过 MarkSent 的话计一次未确认，这是客户端的错，可以接受）。
2. 并发已满：上报 FAILED（让 Manager 立即销账），返回 `RESOURCE_EXHAUSTED`。
3. 入队 RECEIVED 事件（异步，不等发送结果）。
4. `posix_spawn`：stdout、stderr 都接到同一根管道（stderr 对排查失败最有用，`stdout_tail` 里两者混合）；stdin 接 `/dev/null`；`POSIX_SPAWN_SETPGROUP` 让子进程自成进程组，超时时 `kill(-pgid, SIGKILL)` 连孙进程一起杀；`POSIX_SPAWN_SETSIGMASK` 清空信号屏蔽。环境变量只传 `PATH=/usr/local/bin:/usr/bin:/bin` 和 `LM_TASK_ID`。
5. 用 `poll` 读管道，只保留最后 `output_tail_bytes` 字节，同时用 `waitpid(WNOHANG)` 看主进程是否退出；到截止时间杀进程组。主进程退出后再等 100 ms 收尾输出，若仍有后台子孙进程占着管道就杀掉整个进程组：任务以主进程为准，后台进程不能把任务拖到超时。
6. 结果：退出码 0 → DONE；非 0、被信号杀死（exit_code = 128 + 信号）、超时（exit_code = -1）、spawn 失败（exit_code = -2，输出里写错误原因）、取消（exit_code = -3）→ FAILED。入队 DONE/FAILED 事件，`exec_ms` 从 spawn 到 waitpid。
7. 客户端取消（`ctx->IsCancelled()`）：杀进程组，上报 FAILED，返回 `CANCELLED`。

**能力隔离**：Worker 在启动任务服务前调 `prctl(PR_CAP_AMBIENT, PR_CAP_AMBIENT_CLEAR_ALL)`。Worker 自己的 permitted/effective 能力不变（原始 socket、eBPF 在这之前已经打开），之后 exec 的子进程不再继承任何能力。非 Linux 平台无此调用。

**用 posix_spawn 还是 fork**：开发文档要求 `posix_spawn`。它不能在子进程里执行任意代码（比如清能力、chdir），所以能力在父进程里一次性清掉；工作目录沿用 Worker 的（systemd 下是 `/`），`ProtectSystem=strict` 下任务只能写 `/tmp`（PrivateTmp），这对任务是合理的限制。

**Probe**：直接返回请求里的 seq，不校验 token（M6 由 Manager 调用，Manager 也会带 token，但探测本身不执行任何东西）。

### 9. 任务事件发送

- `EventSender`：有界队列（1024，满时丢最旧并计数），一个线程逐条发送 `Collector.OnTaskEvent`，每次截止 500 ms；每个事件最多尝试 3 次（重试前分别等 200 ms、400 ms），仍失败就丢弃并计数、限频记日志。
- 为什么重试：上报样本丢一条，下一秒就有新的；但 DONE 丢了，Manager 要等 300 秒执行超时，还会给这台节点记一次 T，代价大。
- 与上报共用到 Manager 的 channel（同一套重连参数）。

### 10. lm-bench submit

```
lm-bench submit -m <manager> [-r 每秒任务数=5] [-d 秒数=60] [-s 种子=1] [-c client_id]
                [--cmd /bin/sleep] [--args "0.2"] [--bytes 1024] [--token T] [--timeout 30]
```

- 按速率 r 用泊松到达（固定种子的指数分布间隔）发起任务，每个任务一个线程（`std::async`），并发上限 256。
- 每个任务：Allocate → MarkSent → 直连 `worker_addr` 调 Submit（截止 = timeout + 5 s）。Worker 的 stub 按地址缓存。
- 每秒打印一行：各 Worker 地址在这一秒内的分配数和占比、累计完成数、失败数。
- 结束时打印：完成时间（从 Allocate 到 Submit 返回）P50、P99、最大值，按错误码分类的失败数，各节点总占比。也可 `--csv` 输出每任务明细，方便画图。

### 11. 多节点验收环境

- 同一台虚拟机上起第二个 Worker：stress 让两个 Worker 的 CPU 读数一起升高，看不出转移；而且 Manager 用“上报对端 IP + 50052”作为 Worker 地址，同机两个进程地址相同，还得改 proto。不采用。
- 采用：Mac 上的 Docker Desktop 运行第二个 Worker 容器 `lmtest-worker2`（用 `lm-builder` 镜像加 Linux 构建产物，只用于测试，不违背“容器化 Worker 是非目标”）。
  - 它看到的是 Docker Desktop 虚拟机的 /proc，和 lm-worker-1 互不影响。
  - 没有 cpustat 模块和 eBPF、没有原始 socket 能力：`cpu_source: proc`、`runq: off`、`nic_probe: off`，调度延迟和网络两项按 0.3 计，满分只有 0.65。lm-worker-1 满分 1.0，空闲时它约占 60% 的任务。
  - 上报到 `172.16.255.133:50051`，容器发布 50052 端口。Manager 看到的对端 IP 是 Mac 在 VMware NAT 网段的地址，lm-bench 在 Mac 上连这个地址的 50052 就是容器。
  - lm-worker-1 的 `manager_addr` 从 `127.0.0.1:50051` 改为 `172.16.255.133:50051`，这样 Manager 记下的地址是 `172.16.255.133:50052`，Mac 上的 lm-bench 能直连。
- 验收方法：lm-bench 以 10 个/秒提交 `sleep 0.2`，跑 120 秒；第 30–75 秒在 lm-worker-1 上 `stress -c 4`。期望：负载期间 lm-worker-1 的任务占比从约 60% 降到 10% 以下（剔除后为 0），负载结束后 10 秒内回升；全程没有失败。再做一组 `stress -c 2`（CPU 约 0.5，不剔除），占比应部分下降。
- 3 台真实 Worker 到位后（M0 4.2、M1 7.1）重做这组验收。

## 性能与资源影响

- 采集开销、上报频率：不变。
- 调度锁：Allocate 多一次哈希插入和链表尾插；MarkSent、事件各一次哈希查找和 O(1) 链表操作；主循环每 100 ms 一次加锁弹出链表头，每秒一次遍历 20 台节点（每台几次乘法和一次 `exp`）。都在微秒级。
- 内存：每个在途任务一条记录约 150 字节，按 10 个/秒、300 秒执行时限的上限算不到 1 MB。
- Redis：`node:{id}` 每秒多 6 个字段，同一个 pipeline。MySQL 不涉及。
- Worker：任务服务空闲时只有 gRPC 线程；事件发送线程大部分时间阻塞在队列上。

## Risks / Trade-offs

- **Worker 执行远程命令**：默认关闭、白名单、可选 token、子进程无能力、`NoNewPrivileges` 和 `ProtectSystem=strict` 仍然生效。token 走明文 gRPC，内网可以接受，开发文档把认证列为非目标。
- **Docker Desktop 上的 Worker 不是真实节点**：分数天然比 lm-worker-1 低（两项缺失），只能证明“转移”发生，不能用于对照实验的数字。
- **λ、基线下限是拍的**：都放进 `manager.yaml`，对照实验（M10）再调。
- **确认超时的“作废”可能掩盖客户端 bug**：作废次数在汇总日志里单独统计，便于发现。

## Migration Plan

- Manager 先升级：旧 Worker 没有任务服务，客户端 Submit 会失败；这些任务在确认链表里 30 秒后超时（调过 MarkSent 的计一次未确认），不会进入执行链表，`f_timeout` 不受影响。在 Worker 升级前不要对它压测。
- Worker 升级后默认不开任务服务，行为和 M4 一样；要接任务的节点在 `worker.yaml` 里开启 `task.enabled` 并配置白名单。
- 回滚：Manager 换回 `lm-manager:m3` 或 m5a 镜像；Worker 关掉 `task.enabled` 或换回旧二进制。

## 实现中补充的决定（2026-11-13）

### 12. lm-bench 的 `--print`

验收“子进程没有能力”需要看到任务输出，`lm-bench submit` 加了 `--print`，把每个任务的退出码、耗时和输出末尾打印出来。

### 13. Worker 正常重启不会丢事件

`systemctl restart` 时 systemd 先给整个 cgroup 发 SIGTERM，任务进程以 143（128 + 15）退出，Submit 照常返回、FAILED 事件入队；Worker 退出前 `EventSender::stop()` 把队列发完（每条试一次），所以 Manager 立即销账，不会等执行超时。只有 Worker 被 SIGKILL（或断电）时事件才会丢，这时靠 300 秒执行超时兜底并记一次 T。故障路径验收（任务 5.5）因此改用 `systemctl kill -s KILL`，并且直接等 300 秒，不改配置。

### 14. 汇总日志里的“未确认 8”

部署验收时故意提交了 4 个不在白名单的命令和 4 个不带 token 的任务：客户端都调了 MarkSent，Worker 按设计拒绝且不上报事件，30 秒后在 Manager 记为未确认。这是预期行为：未确认统计反映的是“发了却没被 Worker 接收”，原因可能在客户端，也可能在 Worker，M6 的灰色故障判断用的是它相对全体均值的倍数，不是绝对值。
