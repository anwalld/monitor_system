# Spec Delta

## MODIFIED Requirements

### Requirement: Scheduler.Allocate
`Scheduler.Allocate(AllocateReq{client_id, input_bytes, kind, policy})` SHALL 与 `Collector` 在同一端口（默认 50051）提供，返回 `AllocateResp{task_id, worker_addr}`：
- `worker_addr` SHALL 与该节点 `node:{id}` 中的 `addr` 相同（上报对端 IP + `worker_port`）。
- `task_id` SHALL 为 `<Manager 启动时 Unix 毫秒的十六进制>-<从 1 开始的十进制序号>`，同一 Manager 进程内不重复。
- `client_id` 为空或超过 64 字节时 SHALL 返回 `INVALID_ARGUMENT`。
- `policy` 为空或 `score` 时 SHALL 按“候选集合与权重”“加权随机抽样”选择；为 `score_nopred`、`rr`、`wrr` 时 SHALL 按“对照分配策略”选择；其他取值 SHALL 返回 `INVALID_ARGUMENT`。`allocation.allow_policy` 为 false 时，非默认策略 SHALL 返回 `FAILED_PRECONDITION`。
- 没有候选或总权重为 0 时 SHALL 返回 `RESOURCE_EXHAUSTED`，错误信息包含节点总数、剔除数、失联数。
- 无论哪种策略，选中后 SHALL 同样记入在途账（`manager-inflight`）并产生任务记录。
- 处理过程中 MUST NOT 做任何 IO（不访问 Redis、MySQL）。

`Scheduler.MarkSent(TaskRef{task_id})` SHALL 按 `manager-inflight` 的定义实现（把在途任务标为已发送）。

#### Scenario: 正常分配
- **WHEN** 只有 w1 在线且分数为 0.8，地址 10.0.0.1:50052，客户端以 `client_id = "bench"` 调用 Allocate
- **THEN** 返回 OK，`worker_addr` 为 `10.0.0.1:50052`，`task_id` 形如 `<十六进制>-1`；再次调用得到 `<同一前缀>-2`

#### Scenario: 没有可用节点
- **WHEN** 唯一的节点已剔除
- **THEN** 返回 `RESOURCE_EXHAUSTED`，信息中包含“总数 1”“剔除 1”

#### Scenario: 参数错误
- **WHEN** `client_id` 为空，或 `policy` 为 `foo`
- **THEN** 返回 `INVALID_ARGUMENT`

#### Scenario: 关闭对照策略
- **WHEN** `allocation.allow_policy` 为 false，请求 `policy = "rr"`
- **THEN** 返回 `FAILED_PRECONDITION`；`policy` 为空时照常分配

#### Scenario: Redis 不可用
- **WHEN** Redis 已停止，w1 正常上报
- **THEN** Allocate 照常返回 w1

## ADDED Requirements

### Requirement: 对照分配策略
用于对照实验的三种策略 SHALL 只改变“选哪台”，其余流程与默认策略相同：
- `score_nopred`：候选集合与默认策略相同，权重中的 `f_pred` 恒取 1。
- `rr`、`wrr` 的候选集合 SHALL 是距上次上报不超过 3000 ms、且低频探测连续失败次数未达到 `breaker.probe_fail_evict` 的节点；MUST NOT 考虑分数、关键指标剔除、急救、慢启动和任何修正因子。
- `rr`：候选按 `node_id` 排序，Manager 内一个计数器每次加 1，选第 `计数器 mod 候选数` 个。
- `wrr`：平滑加权轮询，权重为节点上报的核数（为 0 时按 1）：每次每个候选 `current += weight`，选 `current` 最大者（相同时取 `node_id` 小的），被选者 `current -= 总权重`；节点不在候选集合时清除其 `current`。
- 计数器与 `current` SHALL 在节点表锁内读写。

#### Scenario: 轮询
- **WHEN** w1、w2、w3 都在上报，w2 已被关键指标剔除，连续以 `rr` 调用 6 次
- **THEN** 依次返回 w1、w2、w3、w1、w2、w3

#### Scenario: 轮询跳过失联节点
- **WHEN** w2 超过 3000 ms 没有上报
- **THEN** `rr` 只在 w1、w3 之间轮换

#### Scenario: 加权轮询
- **WHEN** w1 4 核、w2 2 核，连续以 `wrr` 调用 6 次
- **THEN** w1 被选中 4 次、w2 被选中 2 次，且顺序为 w1、w2、w1、w1、w2、w1

#### Scenario: 不带预测
- **WHEN** w1、w2 分数都是 1，推来 w1 因子 0.5
- **THEN** `score` 选中 w1 的概率约 1/3，`score_nopred` 约 1/2
