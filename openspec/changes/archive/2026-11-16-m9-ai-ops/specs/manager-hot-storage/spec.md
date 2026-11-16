# Spec Delta

## ADDED Requirements

### Requirement: 节点 Hash 中的预测因子
`node:{id}` Hash SHALL 包含 `f_pred`（当前生效的预测因子，无预测或过期时为 1）；有未过期预测时包含 `pred_cpu`（预测的 30 秒后 CPU 忙碌度），否则删除该字段。

#### Scenario: 预测过期
- **WHEN** 最近一次预测已过期后节点再次上报
- **THEN** Hash 里 `f_pred = 1`，没有 `pred_cpu`
