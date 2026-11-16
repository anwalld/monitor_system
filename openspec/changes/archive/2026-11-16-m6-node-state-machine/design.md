# Design

## Context

- `NodeTable` 由一把锁保护，里面有每节点的样本、分数、关键指标剔除状态（`EvictionTracker`），以及在途账 `InflightLedger`（在途量、T、基线）。
- `SchedulerLoop` 每 100 ms 处理在途账链表头，每秒更新基线、衰减 T。
- Worker 的 `TaskService` 已实现 `Submit`、`Probe`；`Rescue` 走基类默认实现（UNIMPLEMENTED）。
- Worker 的 `IntervalGuard`（M2）能在作用域内把 cpustat 刷新周期改成指定值，离开时恢复；进程崩溃时由内核模块在文件释放时恢复默认周期。
- 采集与上报之间的环形缓冲是单生产者单消费者，急救线程不能再从里面取样本。

## Goals / Non-Goals

**Goals**
- 状态机全部是锁内纯内存逻辑，时钟由调用方传入，表驱动单测覆盖全部转移、退避和急救期间失联。
- 急救的判定逻辑（超时、网络事件、批结算、目标）与 gRPC 分离，能用模拟时钟单测。
- 在 1 台虚拟机 + Mac 上 1 个测试 Worker 容器上完成故障注入表中的节点类故障。

**Non-Goals**
- f_pred（M9）、事件推送（M7）、参数调优（M10）。

## Decisions

### 1. 目录与模块

```
manager/breaker/
  breaker_params.{h,cpp}  BreakerParams：breaker 段解析与校验
  node_state.{h,cpp}      NodeBreaker：单节点状态、转移判定（纯逻辑）
  rescue_logic.{h,cpp}    RescueTracker（待确认队列、单次超时、网络事件、批结算）、RescueTarget（目标与 k）、Backoff
  rescue_session.{h,cpp}  RescueSession：一条 Rescue 双向流的发送线程与接收线程
  rescue_manager.{h,cpp}  RescueManager：启动、回收急救会话
  prober.{h,cpp}          Prober：低频探测线程（gRPC 异步 + CompletionQueue）
worker/task/
  rescue_sampler.{h,cpp}  RescueSampler：急救用的独立 CPU 来源 + 刷新周期守卫 + 最新样本
  task_service.*          增加 Rescue
worker/collector/collector.*  增加 latest()：最新样本的加锁副本
```

### 2. 状态与转移

`NodeState` 增加 `NodeBreaker breaker`。状态枚举 `kSlowStart`、`kNormal`、`kEvicted`、`kRescue`，记录 `since_ms`（进入当前状态的单调时钟）和 `reason`。

剔除条件（任一满足）及原因串：

| 条件 | reason |
|---|---|
| 距上次上报 > 3000 ms | `lost` |
| 关键指标剔除（m5a 的 EvictionTracker） | `cpu`、`runq`、`mem`、`net` |
| 低频探测连续失败 ≥ 3 次 | `probe` |
| 最近 5 分钟未确认比例 > 3 × 全体均值，且 ≥ 20 个样本，且比例 ≥ 0.2 | `unconfirmed` |

状态机在调度主循环里每 100 ms 评估一次（`NodeTable::tick_states`），另外两个时机也会立即评估该节点：上报被接受时（关键指标刚越限要马上剔除）、探测结果到达时。

| 当前 | 条件 | 下一个 | 附带动作 |
|---|---|---|---|
| （无） | 第一次上报 | 慢启动 | |
| 慢启动 | 任一剔除条件 | 剔除 | 开发文档只列了“正常 → 剔除”；慢启动中的节点同样是候选，出故障也必须剔除 |
| 慢启动 | 满 `slow_start_s`（300 s） | 正常 | |
| 正常 | 任一剔除条件 | 剔除 | |
| 正常 | 急救条件（第 6 节） | 急救 | 记退避；锁外启动急救会话 |
| 急救 | 会话结束，节点新鲜 | 慢启动 | |
| 急救 | 会话结束，节点已失联 | 剔除（lost） | 急救期间失联不立即剔除 |
| 剔除 | 新鲜、关键指标未剔除、进入剔除后连续 5 次正常上报、探测连续失败数为 0、不满足灰色条件 | 慢启动 | 清空在途基线与 T（`InflightLedger::reset_node`），按新节点对待 |

- 急救期间不评估剔除条件（文档：急救期间失联不立即剔除），结束时再判断新鲜度。关键指标越限也等急救结束后再说：急救最长 10 秒，这期间节点不是候选，不会接任务。
- “连续 5 次正常上报”：进入剔除时清零，此后每条被接受的上报若关键指标未处于剔除状态就加一，否则清零。
- 在途账中已经分配给该节点的任务不受状态变化影响，照常超时或销账。

### 3. 候选与因子

- 候选：状态为正常或慢启动，且（为防止两次评估之间的空窗）新鲜、关键指标未剔除、分数 > 0。
- `f_slow`：慢启动中为 `from + (1 − from) × min(1, 已过时间 / slow_start_s)`，`from = 0.2`；其他状态为 1。
- 慢启动中 `f_inflight` 固定为 1：开发文档说“慢启动期间在途系列的二次调整暂停，只积累基线”。新节点基线为 0，若照常计算，f_slow 本来已经压低了权重，f_inflight 会再压一次，形成二次调整。基线照常每秒更新。f_timeout 照常生效（剔除恢复时 T 已清零；急救结束时 T 已被急救压低）。

### 4. 低频探测

- `Prober` 线程每 `probe_interval_ms`（1000）一轮：锁内取出所有“新鲜且不在急救”的节点的 node_id 和地址（剔除状态的也探测，否则灰色故障恢复不了），锁外对每个地址发一个异步 `Probe`（截止 `probe_timeout_ms` = 500），用一个 CompletionQueue 收齐结果，再逐个 `NodeTable::on_probe(node_id, ok)`。
- 每个地址缓存一个 channel 和 stub；地址变化时换新的。
- `probe_fail_streak`：成功清零，失败加一；≥ 3 剔除。失联节点不探测（已经因失联剔除，探测只会增加噪声）。
- Probe 走和 Submit 同一个 gRPC 服务和线程池，所以“接收任务的链路坏了，它也会失败”。
- 只有开启了任务服务的 Worker 才能通过探测，这是有意的：没有任务服务的节点接不了任务。

### 5. 灰色故障：未确认比例

- `InflightLedger` 为每个节点记录最近 `gray_window_s`（300 s）内的确认结果：RECEIVED 到达（从 ALLOCATED 或 SENT）记一次“确认”；SENT 确认超时记一次“未确认”；ALLOCATED 作废不记（客户端没发）。
- `tick_states` 先修剪窗口，再算全体合并比例 `mean = Σ未确认 / Σ样本`，节点比例 `r` 满足 `样本 ≥ 20` 且 `r > 3 × mean` 且 `r ≥ gray_min_ratio`（0.2）时视为灰色故障。
- `gray_min_ratio` 是本设计补的：全体都健康时 mean 很小，某节点偶尔一个未确认（例如客户端在 MarkSent 之后崩了）就会超过 3 倍均值，需要一个绝对下限。
- 节点被剔除后不再接任务，窗口里的旧样本要在 5 分钟内逐步过期，灰色条件才会解除。这意味着因未确认被剔除的节点至少隔离到坏样本过期，符合预期。

### 6. 急救触发与退避

- 条件（全部满足）：状态正常；`score ≥ 0.6`；`f_timeout < 1` 且 `f_timeout ≤ min(f_inflight, f_slow, f_pred)`；选中概率 `p = w / Σw < 0.5 / N`（N = 候选数，w 按当前因子计算）；`now ≥ next_rescue_ms`。
- 退避：每节点 `level`（实数）与 `level_ts`。触发时先把 level 按半衰期 1 天衰减到当前，间隔 `min(backoff_min × 2^floor(level), backoff_max)`（5 分钟起，60 分钟封顶），`next_rescue_ms = now + 间隔`，然后 `level += 1`。第一次急救不需要等待。
- 进入急救后锁外由 `RescueManager` 启动会话。同一节点同时最多一个会话。

### 7. 急救会话

**Manager 端（`RescueSession`）**
- 单独 channel（不和探测共用，避免互相排队），打开 `Rescue` 双向流，两个线程：
  - 发送线程：按绝对时间每 1 ms 写一条 `RescueReq{seq, send_ts_us}`；每 100 ms 结算一批；判断结束；结束时 `WritesDone` 并 `TryCancel`。
  - 接收线程：`Read` 回应，交给 `RescueTracker::on_response(seq, now_us)`。
- `RescueTracker`（纯逻辑，单测）：
  - 待确认队列先进先出。回应 seq 等于队头则出队记成功；seq 大于队头（中间有丢失）则把它之前的都按超时出队、它本身记成功；seq 小于队头（迟到）丢弃并计数。
  - `check(now)`：队头发送时间超过 10 ms 就出队记一次超时，循环直到队头未超时。
  - 网络事件：一次超时发生时若当前不在“中断中”，事件数加一并进入中断；任何一次成功退出中断。所以连续一段超时、又同时恢复只算一次事件。
  - `settle()`：返回本批是否有超时（有即失败）并清零批计数。
- `RescueTarget`：成功批 `k = 0`；失败批 `k += 1`。目标 `target = max(0, 1/N − ε·(2ᵏ − 1))`，`ε = epsilon_factor / N`，`epsilon_factor = 0.125`（开发文档没给 ε；取 1/(8N) 时连续 4 批失败目标降到 0）。
- 每批结算加锁调用 `NodeTable::rescue_batch(node_id, ok)`：成功时 T × 0.9；返回当前选中概率 p（按急救结束后假设回到候选计算）和 N。`p ≥ target` 或满 10 秒就结束。
- 结束时调用 `NodeTable::end_rescue(node_id, now)`，按第 2 节转为慢启动或剔除；打一条日志：时长、批数（成功/失败）、请求数、成功、超时、迟到、网络事件数、最终 T。
- 流建立失败（Worker 不可达）：所有请求都会超时，批批失败，4 批后目标降到 0、`p ≥ 0` 成立，急救在约 0.4 秒内结束，随后按新鲜度判定。这符合“失联节点不急救”的精神，也不需要特殊分支。

**Worker 端（`TaskService::Rescue` + `RescueSampler`）**
- 同一时刻只允许一条急救流（`try_lock`），第二条返回 `ABORTED`。
- 第一条请求到达时 `RescueSampler::begin()`：创建 `IntervalGuard(1000 µs)`（来源是共享页时才真正改周期），并给独立的 CPU 来源建基线。
- 每条请求：重新读一次 CPU（共享页约 12 ns；/proc 来源时代价大一些但只在测试容器上），用独立的 `CpuBusyCalculator` 算忙碌度；有新数据就覆盖最新样本的 `cpu_busy`、`core_busy`，`ts_ms` 设为当前时间，其余字段（内存、磁盘、调度延迟、网卡）用采集线程最近一次样本的副本（`Collector::latest()`）。
  - 开发文档说“走一遍真实采集链路”。CPU 是 1 ms 周期下唯一会变的高频量；内存、磁盘、网卡按 1 秒窗口统计，每毫秒重读既没有新信息，又要每秒做上千次 /proc 解析。所以只重读 CPU。
- 流结束或超过 `task.rescue_max_s`（15 s）时析构 guard，周期恢复为原值。超过上限时返回 `DEADLINE_EXCEEDED`，防止误用的客户端把节点长期留在 1 ms 周期。
- `Rescue` 和 `Probe` 一样不校验 token：它只读数据、只临时改刷新周期，而且有时长上限；Manager 也不需要持有 Worker 的 token。

### 8. 线程归属

| 线程 | 做什么 | 持锁 |
|---|---|---|
| gRPC（Report） | apply 样本 → 评分、关键指标、good_reports、立即评估该节点 | 节点表锁 |
| 调度主循环（100 ms） | 在途链表头、每秒基线与 T、`tick_states`（含灰色计算、急救触发）；锁外启动急救会话、回收已结束的会话、推送状态变化到存储队列 | 节点表锁 |
| 探测线程（1 s） | 锁内取地址，锁外异步 Probe，结果逐个加锁写回 | 节点表锁（短） |
| 急救线程（每会话 2 个） | 1 ms 发请求、收回应；每 100 ms 加锁结算一次 | 会话自己的锁；结算时节点表锁 |
| 存储线程 | 写 `node:{id}`（含状态变化的单独写入） | 无 |

### 9. 写入 Redis

- 每条上报的 HSET 增加：`state`（`slow_start` / `normal` / `evicted` / `rescue`）、`state_reason`、`state_since_ms`（墙上时间）、`f_slow`（4 位小数）、`probe_fail_streak`。
- 状态变化时，`tick_states` 等把 `StateChange{node_id, state, reason, since_wall_ms, f_slow}` 收集起来，锁外作为一条“只含状态”的 `AcceptedSample`（`state_only = true`）推进存储队列；存储线程对它只写这几个字段，不写时间序列和流。这样失联节点的 `state` 也会变成 `evicted`。

### 10. 配置

```yaml
breaker:
  slow_start_s: 300
  slow_start_from: 0.2
  recover_reports: 5
  probe_interval_ms: 1000
  probe_timeout_ms: 500
  probe_fail_evict: 3
  gray_window_s: 300
  gray_min_samples: 20
  gray_factor: 3
  gray_min_ratio: 0.2
  rescue:
    min_score: 0.6
    prob_factor: 0.5        # p < prob_factor / N
    interval_ms: 1
    timeout_ms: 10
    batch_ms: 100
    max_s: 10
    t_decay: 0.9
    epsilon_factor: 0.125   # ε = epsilon_factor / N
    backoff_min_s: 300
    backoff_max_s: 3600
    backoff_half_life_s: 86400
```

`task.rescue_max_s: 15`（Worker）。

## 性能与资源影响

- 采集开销：平时不变；急救期间（≤ 10 s）cpustat 1 ms 刷新、每秒 1000 次共享页读取和 1000 条回应。
- 上报频率不变。Redis：每条上报多 5 个字段；状态变化额外写一次。MySQL 不涉及。
- 调度锁：`tick_states` 每 100 ms 遍历 ≤ 20 台节点，灰色计算修剪每节点一个 deque；探测每节点每秒一次加锁；急救每 100 ms 一次加锁。
- Manager 线程：+1 探测线程，急救时每节点 +2 线程。

## Risks / Trade-offs

- **急救阈值依赖 N**：只有 2 台节点时 `0.5/N = 0.25`，T 要累计到 3 以上才会触发急救；节点多时更容易触发。按文档实现，对照实验再调。
- **目标降到 0 时急救会很快结束**：坏节点（或间歇故障节点）会在不到 1 秒内结束急救进入慢启动，此后再靠剔除条件和退避兜底。急救的价值在于“发现”和“健康时快速恢复 T”，而不是长期隔离。
- **测试环境**：第二个节点是 Mac 上的 Docker 容器，“关掉 Worker 虚拟机”会连 Manager 一起关掉（同一台虚拟机），所以节点宕机用停止 Worker 服务（`systemctl stop lm-worker`）模拟：从 Manager 看是一样的（上报和探测都停了）。
- **未确认的绝对下限 0.2 是拍的**：放进配置。

## Migration Plan

- Manager 升级后，所有已上报的节点在下一条上报时作为新节点进入慢启动（状态只在内存里），5 分钟后回到正常。升级期间这些节点权重从 0.2 起步，不影响可用性。
- Worker 需要升级到本版本并开启任务服务，否则探测失败被剔除。先升级 Worker，再升级 Manager。
- 回滚：Manager 换回 `lm-manager:m5` 镜像；Worker 换回 M5 二进制（Manager 回滚后不会调 Rescue）。

## 实现中补充的决定（2026-11-14）

### 11. “达到目标”允许 1% 相对误差

急救每个成功批只把 T 乘以 0.9，选中概率只能渐近 1/N：两台分数相同的节点，p 永远到不了 0.5。所以判定为 `p ≥ target × 0.99`（`RescueTarget::reached`）。虚拟机上实测 p 0.503 / 目标 0.500 时结束。

### 12. 退避级别从退避窗口结束时开始衰减

如果从触发时刻起就按 1 天半衰期衰减，5 分钟退避窗口里 level 会从 1 掉到 0.998，`floor` 落后一档，间隔变成 5、5、10、20…，与规格不符。改为退避窗口内不衰减、从窗口结束时刻开始衰减：连续急救时严格 5、10、20、40、60、60 分钟；长时间没有急救时照常衰减。

### 13. Worker 进程停止时，先触发的通常是探测剔除

进程停止后，任务端口和上报同时消失。探测每秒一次，第 3 次失败约在 2–3 秒，往往早于 3 秒失联判定，所以原因记为 `probe`（实测 2.7 秒）。两者都满足“3 秒内剔除、不急救”的要求，规格里补了这个场景。

### 14. 测试用的 T 怎么来

开发文档的急救要求 f_timeout 是各因子里最小的。测试时让 Mac 上的测试 Worker 暂停几秒（剔除，不接任务），向 lm-worker-1 提交 8 个 `sleep 400`（Submit 的 timeout_s 也是 400，比 300 秒执行超时长）。300 秒后 Manager 记 8 次执行超时，不需要杀进程，也不会因失联清空 T。
