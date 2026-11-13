# Spec Delta

## Purpose

定义 Worker 向 Manager 上报节点样本和任务事件的协议契约，包括字段语义、取值范围和缺省语义，使双方可以独立演进而不破坏兼容。

## ADDED Requirements

### Requirement: 样本字段语义
上报样本 SHALL 包含：`node_id`（节点标识）、`seq`（同一 Worker 进程内从 1 单调递增）、`ts_ms`（采集时刻，Unix 毫秒）、`cpu_busy`（全机忙碌度 0~1）、`core_busy`（每核忙碌度 0~1，按 CPU 编号排列）、`mem_avail_ratio`（0~1）、`disk_util`（0~1）、`nics`（每块网卡一项）。所有比例字段 MUST 截断到 [0, 1]。

#### Scenario: 合法样本
- **WHEN** Worker 在 4 核主机上发出一条样本
- **THEN** `core_busy` 有 4 个元素，所有比例字段都在 [0, 1] 内，`seq` 大于该进程上一条样本的 `seq`

### Requirement: 可选指标区分“未上报”与 0
调度延迟相关字段 `runq_p50_us`、`runq_p99_us`、`runq_drop_rate` MUST 为可选字段：未采集时不设置，接收方 SHALL 能区分“未上报”和“值为 0”。

#### Scenario: 未采集调度延迟
- **WHEN** Worker 未启用调度延迟采集并发出样本
- **THEN** Manager 侧对这三个字段的存在性判断均为“不存在”，而不是读到 0

### Requirement: Report 为一元调用且不保证送达
`Collector.Report` SHALL 为一元调用，Manager 处理成功返回空 Ack。调用方 MUST 设置截止时间（上报周期的一半），超时或失败不重传。

#### Scenario: 正常上报
- **WHEN** Manager 正常运行时 Worker 调用 Report
- **THEN** 在截止时间内收到 OK 状态与空 Ack

#### Scenario: 缺少 node_id
- **WHEN** 收到 `node_id` 为空的样本
- **THEN** Manager 返回 INVALID_ARGUMENT，且不更新任何节点状态

### Requirement: 任务事件接口
`Collector.OnTaskEvent` SHALL 接收任务事件（`task_id`、`kind` 为 RECEIVED/DONE/FAILED、`ts_ms`、`exec_ms`）并返回空 Ack；`kind` 为 KIND_UNSPECIFIED 时 MUST 返回 INVALID_ARGUMENT。

#### Scenario: 收到合法事件
- **WHEN** 调用 OnTaskEvent，kind 为 RECEIVED
- **THEN** 返回 OK 与空 Ack

#### Scenario: 未指定事件类型
- **WHEN** 调用 OnTaskEvent，kind 为 KIND_UNSPECIFIED
- **THEN** 返回 INVALID_ARGUMENT

### Requirement: 协议演进保持兼容
协议中已发布的字段号和服务方法 MUST NOT 被复用或改变含义；新增字段只能追加新字段号。

#### Scenario: 新旧版本混跑
- **WHEN** 只认识 M1 字段的 Worker 向追加了新字段的 Manager 上报
- **THEN** Manager 正常处理，新字段按“未上报”对待
