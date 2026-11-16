# Spec Delta

## MODIFIED Requirements

### Requirement: 候选集合与权重
候选节点 SHALL 是状态为正常或慢启动、距上次上报不超过 3000 ms、关键指标未剔除、分数大于 0、且在途未满的节点；“在途未满”指节点最近一次上报的 `max_concurrent` 为 0，或 Manager 在途账中该节点的任务数（已分配、已发送、运行中）小于 `max_concurrent`。在途已满只影响本次选择，MUST NOT 改变分数、剔除与节点状态，也不计入状态机使用的选中概率；剔除和急救状态的节点 MUST NOT 被选中。每个候选的权重 SHALL 为 `w = score × f_inflight × f_timeout × f_slow × f_pred`：`f_inflight`、`f_timeout` 按 `manager-inflight` 计算（慢启动中 `f_inflight` 取 1），`f_slow` 按 `manager-node-state` 计算，`f_pred` SHALL 取 `Advisor.PushPredictions` 最近一次推来的该节点因子，限幅到 [`advisor.min_factor`（默认 0.5）, 1.0]；预测过期（距推送超过 `ttl_ms`，ttl 为 0 时按 10000、上限 `advisor.max_ttl_ms` 60000）或从未收到时为 1.0。

#### Scenario: 剔除与失联的节点不参与
- **WHEN** 节点 w1 正常（分数 0.8），w2 已剔除，w3 失联
- **THEN** 候选集合只有 w1

#### Scenario: 超时多的节点少分
- **WHEN** w1、w2 分数都是 1，w1 的 T 为 2、w2 的 T 为 0，两者都没有在途任务
- **THEN** w1 被选中的概率为 exp(−1) / (exp(−1) + 1) ≈ 0.269

#### Scenario: 慢启动节点少分
- **WHEN** w1 正常（score 1），w2 刚进入慢启动（score 1）
- **THEN** w2 被选中的概率为 0.2 / 1.2 ≈ 0.167

#### Scenario: 急救中的节点
- **WHEN** w1 处于急救，w2 正常
- **THEN** Allocate 总是返回 w2

#### Scenario: 收到预测
- **WHEN** w1、w2 分数都是 1、其他因子为 1，推来 w1 因子 0.5、ttl 10 秒
- **THEN** 10 秒内 w1 被选中的概率约为 1/3，10 秒后回到 1/2

#### Scenario: 因子越界
- **WHEN** 推来因子 0.1 或 1.5
- **THEN** 记录为 0.5 和 1.0

#### Scenario: 在途已满的节点不参与
- **WHEN** w1 上报 `max_concurrent = 2`，已为它分配 2 个尚未结束的任务；w2 正常
- **THEN** 下一次 Allocate 返回 w2；w1 的一个任务完成（DONE 事件）后，w1 重新参与选择

#### Scenario: 不限制
- **WHEN** w1 上报 `max_concurrent = 0`，在途 100 个任务
- **THEN** w1 仍是候选

### Requirement: Scheduler.Allocate
`Scheduler.Allocate(AllocateReq{client_id, input_bytes, kind, policy})` SHALL 与 `Collector` 在同一端口（默认 50051）提供，返回 `AllocateResp{task_id, worker_addr}`：
- `worker_addr` SHALL 与该节点 `node:{id}` 中的 `addr` 相同（上报对端 IP + `worker_port`）。
- `task_id` SHALL 为 `<Manager 启动时 Unix 毫秒的十六进制>-<从 1 开始的十进制序号>`，同一 Manager 进程内不重复。
- `client_id` 为空或超过 64 字节时 SHALL 返回 `INVALID_ARGUMENT`。
- `policy` 为空或 `score` 时 SHALL 按“候选集合与权重”“加权随机抽样”选择；为 `score_nopred`、`rr`、`wrr` 时 SHALL 按“对照分配策略”选择；其他取值 SHALL 返回 `INVALID_ARGUMENT`。`allocation.allow_policy` 为 false 时，非默认策略 SHALL 返回 `FAILED_PRECONDITION`。
- 没有候选或总权重为 0 时 SHALL 返回 `RESOURCE_EXHAUSTED`，错误信息包含节点总数、剔除数、失联数，以及在途已满的节点数。
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
