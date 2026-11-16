# Spec Delta

## MODIFIED Requirements

### Requirement: 样本字段语义
上报样本 SHALL 包含：`node_id`（节点标识）、`seq`（同一 Worker 进程内从 1 单调递增）、`ts_ms`（采集时刻，Unix 毫秒）、`cpu_busy`（全机忙碌度 0~1）、`core_busy`（每核忙碌度 0~1，按 CPU 编号排列）、`mem_avail_ratio`（0~1）、`disk_util`（0~1）、`nics`（每块网卡一项）、`max_concurrent`（任务服务的并发上限，0 表示未开任务服务或不限制）。所有比例字段 MUST 截断到 [0, 1]。

#### Scenario: 合法样本
- **WHEN** Worker 在 4 核主机上发出一条样本
- **THEN** `core_busy` 有 4 个元素，所有比例字段都在 [0, 1] 内，`seq` 大于该进程上一条样本的 `seq`

#### Scenario: 并发上限
- **WHEN** Worker 开启任务服务、`task.max_concurrent` 为 4
- **THEN** 每条样本的 `max_concurrent` 为 4；关闭任务服务时为 0
