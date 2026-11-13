# Spec Delta

## ADDED Requirements

### Requirement: 节点 Hash 中的分数明细与剔除状态
每次接受上报后写 `node:{id}` 时，Manager SHALL 在同一个 HSET 中写入：`score`、`s_cpu`、`s_runq`、`s_mem`、`s_net`（小数点后 4 位，缺失项写实际计入的分数）、`best_nic`（没有网卡时为空串）、`evicted`（`0` 或 `1`）、`evict_reason`（剔除的指标名，多个用逗号连接，未剔除时为空串）。原有字段 MUST 保持不变。

#### Scenario: 正常节点
- **WHEN** 节点 w1 上报一条各项满分、网卡 ens160 的样本
- **THEN** `HGETALL node:w1` 中 `score` 为 `1.0000`，`best_nic` 为 `ens160`，`evicted` 为 `0`，`evict_reason` 为空

#### Scenario: 剔除节点
- **WHEN** w1 的 CPU 忙碌度连续 3 次为 0.98
- **THEN** 第 3 次之后 `node:w1` 中 `evicted` 为 `1`，`evict_reason` 为 `cpu`
