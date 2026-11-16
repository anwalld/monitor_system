# Spec Delta

## Purpose

Manager 低频探测每台 Worker 的任务端口，发现“上报正常、但接不了任务”的灰色故障。

## ADDED Requirements

### Requirement: 低频探测
Manager SHALL 每 1000 ms 对每台距上次上报不超过 3000 ms、且不在急救中的节点（包括剔除状态的节点）调用一次 `WorkerTask.Probe(ProbeReq{seq})`，截止 500 ms。返回 OK 且 seq 一致计为成功，否则计为失败。成功 SHALL 把该节点的连续失败数清零，失败 SHALL 加一。探测 MUST 异步进行，一台节点超时不得推迟其他节点的探测。

#### Scenario: 端口被丢包
- **WHEN** w1 的 50052 端口入站被丢弃，上报照常
- **THEN** 约 3 秒后 w1 的 `probe_fail_streak` 达到 3，状态变为 `evicted`，原因 `probe`

#### Scenario: 恢复
- **WHEN** 上述丢包规则删除
- **THEN** 下一次探测成功后连续失败数为 0，满足其余恢复条件后 w1 进入慢启动

#### Scenario: 互不影响
- **WHEN** w1 探测超时，w2 正常
- **THEN** 同一轮中 w2 的探测在 500 ms 内完成并计为成功
