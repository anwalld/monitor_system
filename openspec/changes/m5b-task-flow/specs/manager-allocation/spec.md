# Spec Delta

## ADDED Requirements

### Requirement: 选择权重使用在途与超时因子
自本变更起，候选节点权重 `w = score × f_inflight × f_timeout × f_slow × f_pred` 中的 `f_inflight`、`f_timeout` SHALL 按 `manager-inflight` 的定义计算；`f_slow`、`f_pred` 仍取 1。`Scheduler.MarkSent` SHALL 按 `manager-inflight` 实现，不再返回 `UNIMPLEMENTED`。

#### Scenario: 超时多的节点少分
- **WHEN** w1、w2 分数都是 1，w1 的 T 为 2、w2 的 T 为 0，两者都没有在途任务
- **THEN** w1 被选中的概率为 exp(−1) / (exp(−1) + 1) ≈ 0.269
