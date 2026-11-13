# Spec Delta

## Purpose

Manager 记录每个已分配任务走到了哪一步，据此统计每台节点的在途量、未确认数和执行超时数，并把它们反映到选择权重里。

## ADDED Requirements

### Requirement: 分配时记账
`Allocate` 选中节点后 SHALL 在同一次加锁内为任务建账：状态 ALLOCATED，放入确认链表，截止时间为分配时刻 + 30 s；该节点的总数、在途任务数各加一，在途字节数加 `input_bytes`。

#### Scenario: 分配
- **WHEN** 客户端以 `input_bytes = 4096` 调用 Allocate 并选中 w1
- **THEN** w1 的在途任务数为 1、在途字节数为 4096，任务处于确认链表

### Requirement: MarkSent
`Scheduler.MarkSent(TaskRef{task_id})` SHALL：
- task_id 为空时返回 `INVALID_ARGUMENT`；不存在时返回 `NOT_FOUND`。
- 任务处于 ALLOCATED 时改为 SENT，移到确认链表队尾，截止时间重置为当前时刻 + 30 s，返回 OK。
- 任务处于 SENT 或 RUNNING 时不改变状态，返回 OK。

#### Scenario: 标记已发送
- **WHEN** 任务 t1 在 0 s 分配，在 20 s 调用 MarkSent
- **THEN** 返回 OK，t1 的确认截止时间变为 50 s

#### Scenario: 不存在的任务
- **WHEN** 对从未分配的 task_id 调用 MarkSent
- **THEN** 返回 `NOT_FOUND`

### Requirement: 任务事件销账
`Collector.OnTaskEvent` SHALL 按 task_id 更新在途账：
- RECEIVED：ALLOCATED 或 SENT 的任务移出确认链表，状态改为 RUNNING，放入执行链表，截止时间为收到时刻 + 300 s；RUNNING 的任务忽略。
- DONE 或 FAILED：无论处于哪个状态，删除任务，节点的在途任务数减一、在途字节数减去该任务的 `input_bytes`。
- 查不到 task_id（迟到或未知）：只增加迟到事件计数，MUST NOT 改变任何节点的计数，返回 OK。
- task_id 为空或 kind 为 `KIND_UNSPECIFIED` 时返回 `INVALID_ARGUMENT`。

#### Scenario: 正常完成
- **WHEN** 任务依次经历 Allocate、MarkSent、RECEIVED、DONE
- **THEN** 节点在途任务数回到 0，未确认数和超时计数都不变

#### Scenario: 没有 RECEIVED 直接完成
- **WHEN** 任务分配后直接收到 FAILED
- **THEN** 任务被删除，在途任务数减一

#### Scenario: 迟到的完成
- **WHEN** 任务已因执行超时被删除，之后收到它的 DONE
- **THEN** 返回 OK，迟到事件计数加一，节点在途数和超时计数都不变

### Requirement: 确认超时
调度主循环 SHALL 每 100 ms 检查确认链表头，截止时间已过的任务 SHALL 删除，节点总数和在途任务数各减一、在途字节数减去其 `input_bytes`：
- 状态为 SENT 的，节点“未确认”计数 SHALL 加一。
- 状态为 ALLOCATED 的只作废，MUST NOT 计入未确认。
确认超时 MUST NOT 增加超时计数 T。

#### Scenario: 发了但没收到
- **WHEN** 任务在 0 s 分配、1 s MarkSent，之后没有任何事件
- **THEN** 在 31 s 到 31.1 s 之间任务被删除，节点未确认数加一，T 不变

#### Scenario: 拿到地址但没发
- **WHEN** 任务在 0 s 分配，之后没有 MarkSent 和任何事件
- **THEN** 在 30 s 到 30.1 s 之间任务被删除，未确认数和 T 都不变

### Requirement: 执行超时
调度主循环 SHALL 每 100 ms 检查执行链表头，截止时间已过的任务 SHALL 删除，节点在途任务数减一、在途字节数减去其 `input_bytes`，超时计数 T 加一。

#### Scenario: 执行超时
- **WHEN** 任务在 10 s 收到 RECEIVED，之后没有 DONE 或 FAILED
- **THEN** 在 310 s 到 310.1 s 之间任务被删除，T 增加 1

### Requirement: 在途因子与超时因子
选择权重中的因子 SHALL 按以下方式计算：
- `f_inflight = 1 / (1 + max(0, r − 1))`，`r = max(在途任务数 / max(任务基线, 2), 在途字节数 / max(字节基线, 1048576))`。基线 SHALL 每秒按 `base ← base + α·(当前在途量 − base)` 更新，`α = 1 − exp(−1/60)`，初值 0。
- `f_timeout = exp(−0.5 · T)`；T SHALL 每秒乘以 `0.5^(1/600)`（半衰期 10 分钟）。
λ、基线时间常数、基线下限、半衰期 SHALL 可在 `manager.yaml` 的 `inflight` 段配置。

#### Scenario: 在途堆积降权
- **WHEN** 节点基线都为 0，在途任务 6 个、在途字节 0
- **THEN** r = 3，f_inflight = 1/3

#### Scenario: 在途不多不降权
- **WHEN** 节点在途任务 2 个，基线为 0
- **THEN** f_inflight = 1

#### Scenario: 超时降权与衰减
- **WHEN** 节点发生 2 次执行超时，之后 600 秒没有新的超时
- **THEN** 刚超时时 f_timeout = exp(−1) ≈ 0.368；600 秒后 T ≈ 1，f_timeout ≈ 0.607

### Requirement: 在途状态写入节点 Hash
每次接受上报后写 `node:{id}` 时，Manager SHALL 在同一个 HSET 中写入 `inflight_tasks`、`inflight_bytes`、`unconfirmed`（整数），`f_inflight`、`f_timeout`、`timeout_count`（小数点后 4 位）。

#### Scenario: 写入
- **WHEN** w1 有 3 个在途任务、T 为 0 时上报一条样本
- **THEN** `node:w1` 中 `inflight_tasks` 为 `3`，`f_timeout` 为 `1.0000`
