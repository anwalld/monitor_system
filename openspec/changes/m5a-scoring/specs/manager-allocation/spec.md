# Spec Delta

## Purpose

客户端向 Manager 申请一台 Worker，Manager 在候选节点中按权重随机选一台，返回任务 ID 和 Worker 地址。

## ADDED Requirements

### Requirement: 候选集合与权重
候选节点 SHALL 是未剔除、未失联、分数大于 0 的节点。每个候选的权重 SHALL 为 `w = score × f_inflight × f_timeout × f_slow × f_pred`；在本变更中四个因子 SHALL 取 1。

#### Scenario: 剔除与失联的节点不参与
- **WHEN** 节点 w1 正常（分数 0.8），w2 已剔除，w3 失联
- **THEN** 候选集合只有 w1

### Requirement: 加权随机抽样
Manager SHALL 按候选节点 `node_id` 排序后计算权重前缀和，取 [0, 1) 均匀随机数 u，选中第一个前缀和大于 `u × 总权重` 的节点。每个节点被选中的概率 SHALL 等于其权重占总权重的比例。

#### Scenario: 频率与权重成比例
- **WHEN** 三个候选的权重为 0.2、0.3、0.5，抽样 100000 次
- **THEN** 各节点被选中的频率与 0.2、0.3、0.5 的绝对误差都小于 0.01

#### Scenario: 边界
- **WHEN** 权重为 1、0、1，u 分别取 0、0.4999、0.5、0.9999
- **THEN** 依次选中第 1、1、3、3 个节点，权重为 0 的节点从不被选中

### Requirement: Scheduler.Allocate
`Scheduler.Allocate(AllocateReq{client_id, input_bytes, kind})` SHALL 与 `Collector` 在同一端口（默认 50051）提供，返回 `AllocateResp{task_id, worker_addr}`：
- `worker_addr` SHALL 与该节点 `node:{id}` 中的 `addr` 相同（上报对端 IP + `worker_port`）。
- `task_id` SHALL 为 `<Manager 启动时 Unix 毫秒的十六进制>-<从 1 开始的十进制序号>`，同一 Manager 进程内不重复。
- `client_id` 为空或超过 64 字节时 SHALL 返回 `INVALID_ARGUMENT`。
- 没有候选或总权重为 0 时 SHALL 返回 `RESOURCE_EXHAUSTED`，错误信息包含节点总数、剔除数、失联数。
- 处理过程中 MUST NOT 做任何 IO（不访问 Redis、MySQL）。

`Scheduler.MarkSent` 在本变更中 SHALL 返回 `UNIMPLEMENTED`。

#### Scenario: 正常分配
- **WHEN** 只有 w1 在线且分数为 0.8，地址 10.0.0.1:50052，客户端以 `client_id = "bench"` 调用 Allocate
- **THEN** 返回 OK，`worker_addr` 为 `10.0.0.1:50052`，`task_id` 形如 `<十六进制>-1`；再次调用得到 `<同一前缀>-2`

#### Scenario: 没有可用节点
- **WHEN** 唯一的节点已剔除
- **THEN** 返回 `RESOURCE_EXHAUSTED`，信息中包含“总数 1”“剔除 1”

#### Scenario: 参数错误
- **WHEN** `client_id` 为空
- **THEN** 返回 `INVALID_ARGUMENT`

#### Scenario: Redis 不可用
- **WHEN** Redis 已停止，w1 正常上报
- **THEN** Allocate 照常返回 w1
