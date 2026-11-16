# Spec Delta

## Purpose

Manager 为每台节点维护一个熔断器式的状态：正常、剔除、急救、慢启动。状态决定节点能否被选中、权重打几折。

## ADDED Requirements

### Requirement: 新节点进入慢启动
节点第一次上报被接受时 SHALL 进入慢启动状态。

#### Scenario: 新节点
- **WHEN** w1 第一次上报
- **THEN** w1 的状态为 `slow_start`，f_slow 为 0.2

### Requirement: 慢启动
慢启动中的节点 SHALL 是候选，`f_slow = 0.2 + 0.8 × min(1, 已过时间 / 300 s)`，`f_inflight` SHALL 固定为 1（在途基线照常更新）。满 300 s 且期间未满足剔除条件的节点 SHALL 转为正常，f_slow 变为 1。

#### Scenario: 线性升权
- **WHEN** w1 进入慢启动 150 s
- **THEN** f_slow 为 0.6

#### Scenario: 慢启动结束
- **WHEN** w1 进入慢启动 300 s，期间一切正常
- **THEN** w1 状态为 `normal`，f_slow 为 1

#### Scenario: 慢启动中不叠加在途降权
- **WHEN** 慢启动中的 w1 有 6 个在途任务、基线为 0
- **THEN** w1 的权重为 score × f_slow × f_timeout，不乘 1/3

### Requirement: 剔除
处于正常或慢启动的节点满足以下任一条件时 SHALL 转为剔除，并记录原因：
- 距上次上报超过 3000 ms（`lost`）；
- 关键指标处于剔除状态（`cpu`、`runq`、`mem`、`net`）；
- 低频探测连续失败 ≥ 3 次（`probe`）；
- 最近 300 s 未确认样本 ≥ 20、未确认比例大于全体合并比例的 3 倍且不小于 0.2（`unconfirmed`）。

失联导致的剔除 MUST NOT 触发急救。剔除状态的节点 MUST NOT 被选中。

#### Scenario: 失联 3 秒内剔除
- **WHEN** w1 在 t 时刻最后一次上报，之后不再上报
- **THEN** 在 t + 3.1 s 之前 w1 状态为 `evicted`，原因 `lost`，期间没有对 w1 发起急救

#### Scenario: Worker 进程停止
- **WHEN** w1 的 Worker 进程停止（上报和任务端口同时消失）
- **THEN** 3 秒内 w1 被剔除，原因为先满足的 `probe`（连续 3 次探测失败）或 `lost`，不发起急救

#### Scenario: 探测连续失败
- **WHEN** w1 照常上报，但连续 3 次低频探测失败
- **THEN** w1 状态为 `evicted`，原因 `probe`

#### Scenario: 未确认比例
- **WHEN** 最近 5 分钟 w1 有 20 个确认样本，其中 10 个未确认；w2 有 100 个确认样本、0 个未确认
- **THEN** 全体比例 10/120 ≈ 0.083，w1 比例 0.5 > 0.25 且 ≥ 0.2，w1 被剔除，原因 `unconfirmed`

#### Scenario: 样本不足
- **WHEN** w1 最近 5 分钟只有 19 个样本且全部未确认
- **THEN** w1 不因未确认比例被剔除

### Requirement: 剔除后恢复
剔除状态的节点同时满足以下条件时 SHALL 转为慢启动：距上次上报不超过 3000 ms；关键指标未处于剔除状态；进入剔除后已连续 5 次被接受的上报关键指标都未剔除；低频探测连续失败数为 0；不满足未确认比例条件。转换时 SHALL 清空该节点的在途基线和超时计数 T。

#### Scenario: 节点恢复
- **WHEN** 因失联被剔除的 w1 重新上报 5 条正常样本，期间探测成功
- **THEN** 第 5 条之后 w1 状态为 `slow_start`，T 为 0，基线为 0

#### Scenario: 恢复中断
- **WHEN** 被剔除的 w1 上报 4 条正常样本后第 5 条 CPU 越限
- **THEN** w1 仍为 `evicted`，正常计数重新开始

### Requirement: 急救期间的失联
急救中的节点 MUST NOT 因任何剔除条件立即剔除。急救结束时若节点距上次上报不超过 3000 ms SHALL 转为慢启动，否则 SHALL 转为剔除（原因 `lost`）。

#### Scenario: 急救中失联
- **WHEN** w1 进入急救后停止上报，急救 10 s 后结束
- **THEN** 急救期间 w1 状态一直是 `rescue`；结束后为 `evicted`，原因 `lost`

### Requirement: 状态写入节点 Hash
Manager SHALL 在 `node:{id}` 中写入 `state`（`slow_start`、`normal`、`evicted`、`rescue`）、`state_reason`、`state_since_ms`、`f_slow`（4 位小数）、`probe_fail_streak`。每条上报的 HSET 带上这些字段；状态变化时 SHALL 立即单独写一次，不等下一条上报。

#### Scenario: 失联节点的状态可见
- **WHEN** w1 停止上报 4 秒
- **THEN** `node:w1` 中 `state` 为 `evicted`，`state_reason` 为 `lost`
