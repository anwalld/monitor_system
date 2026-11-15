# Spec Delta

## ADDED Requirements

### Requirement: 选择权重使用预测因子
自本变更起，候选节点权重 `w = score × f_inflight × f_timeout × f_slow × f_pred` 中的 `f_pred` SHALL 取 `Advisor.PushPredictions` 最近一次推来的该节点因子，限幅到 [`advisor.min_factor`（默认 0.5）, 1.0]；预测过期（距推送超过 `ttl_ms`，ttl 为 0 时按 10000、上限 `advisor.max_ttl_ms` 60000）或从未收到时为 1.0。`Advisor.PushPredictions` SHALL 在节点表锁内只做赋值，未知节点忽略并计数，返回 OK；`advisor.enabled = false` 时直接返回 OK 不记录。

#### Scenario: 收到预测
- **WHEN** w1、w2 分数都是 1、其他因子为 1，推来 w1 因子 0.5、ttl 10 秒
- **THEN** 10 秒内 w1 被选中的概率约为 1/3，10 秒后回到 1/2

#### Scenario: 因子越界
- **WHEN** 推来因子 0.1 或 1.5
- **THEN** 记录为 0.5 和 1.0
