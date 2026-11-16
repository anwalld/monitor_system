# Spec Delta

## ADDED Requirements

### Requirement: 节点 Hash 中的调度延迟字段
样本设置了 `runq_p50_us`、`runq_p99_us`、`runq_drop_rate` 时，Manager SHALL 把它们以同名字段写入 Hash `node:{id}`，与其他字段在同一次更新中写入。样本没有设置这三个字段时，Manager MUST 在同一次更新中删除 Hash 中已有的这三个字段，避免读方看到过时的值。`node:{id}:ts` 与 `stream:metrics` 中的样本 JSON SHALL 在字段被设置时包含它们（包括值为 0 的情况），未设置时不包含。

#### Scenario: 新版 Worker 上报
- **WHEN** w1 上报的样本设置了 `runq_p99_us = 850`
- **THEN** `HGET node:w1 runq_p99_us` 返回 850，`node:w1:ts` 最新成员的 JSON 中有 `"runq_p99_us":850`

#### Scenario: 值为 0 仍写入
- **WHEN** 样本设置了 `runq_drop_rate = 0`
- **THEN** `HGET node:w1 runq_drop_rate` 返回 0，样本 JSON 中包含 `runq_drop_rate`

#### Scenario: 未上报调度延迟
- **WHEN** w2 的样本没有设置三个调度延迟字段
- **THEN** `HEXISTS node:w2 runq_p99_us` 为 0，样本 JSON 中不含这三个字段

#### Scenario: 关闭调度延迟后旧值被清除
- **WHEN** w1 先上报了带 `runq_p99_us` 的样本，之后以 `runq: off` 重启并继续上报
- **THEN** 重启后第一条样本写入后，`HEXISTS node:w1 runq_p99_us` 为 0
