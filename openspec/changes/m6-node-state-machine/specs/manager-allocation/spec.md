# Spec Delta

## ADDED Requirements

### Requirement: 候选集合按节点状态
自本变更起，候选节点 SHALL 是状态为正常或慢启动、距上次上报不超过 3000 ms、关键指标未剔除、分数大于 0 的节点；权重中的 `f_slow` 按 `manager-node-state` 计算，慢启动中 `f_inflight` 取 1，`f_pred` 仍取 1。剔除和急救状态的节点 MUST NOT 被选中。

#### Scenario: 慢启动节点少分
- **WHEN** w1 正常（score 1），w2 刚进入慢启动（score 1）
- **THEN** w2 被选中的概率为 0.2 / 1.2 ≈ 0.167

#### Scenario: 急救中的节点
- **WHEN** w1 处于急救，w2 正常
- **THEN** Allocate 总是返回 w2
