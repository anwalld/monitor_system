# Spec Delta

## MODIFIED Requirements

### Requirement: 候选集合与权重
候选节点 SHALL 是状态为正常或慢启动、距上次上报不超过 3000 ms、关键指标未剔除、分数大于 0 的节点；剔除和急救状态的节点 MUST NOT 被选中。每个候选的权重 SHALL 为 `w = score × f_inflight × f_timeout × f_slow × f_pred`：`f_inflight`、`f_timeout` 按 `manager-inflight` 计算（慢启动中 `f_inflight` 取 1），`f_slow` 按 `manager-node-state` 计算，`f_pred` 取 1。

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
