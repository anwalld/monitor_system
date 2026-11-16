# ai-load-prediction Specification

## Purpose
AI 服务用统计方法预测每台节点 30 秒后的 CPU 忙碌度，换算成调整因子推给 Manager 参与选择；预测带过期，AI 挂掉后权重自动回到原值。

## Requirements

### Requirement: Holt 预测
AI 服务 SHALL 对每台节点的 CPU 忙碌度维护 Holt 线性指数平滑（水平、趋势，阻尼趋势：趋势每秒乘 φ），默认 α = 0.3、β = 0.05、φ = 0.9，每收到一条样本更新一次；样本不少于 30 条后，每 5 秒预测 30 秒后的值，限幅 [0, 1]。不使用 LLM。

#### Scenario: 负载爬升
- **WHEN** 一台节点 CPU 从 0.2 以每秒 0.02 的速度上升
- **THEN** 预测值高于当前值，且在 30 秒后与实际值的绝对误差小于朴素基线（当前值）

### Requirement: 调整因子
预测值 ≤ 0.7 时因子 SHALL 为 1.0；0.7 到 1.0 之间线性降到 0.5；≥ 1.0 为 0.5。因子只降不升（≤ 1.0）。

#### Scenario: 因子换算
- **WHEN** 预测值为 0.85
- **THEN** 因子为 0.75

### Requirement: 推送与过期
`predict.enabled` 为 true 时，AI 服务 SHALL 每 5 秒为最后样本不超过 10 秒的节点写 Redis `ai:pred`（Hash，字段为节点 id，值含 cpu_busy_30s、factor、ts_ms，10 秒过期）。`predict.push` 也为 true 时（默认 false，见“基线对比”），SHALL 同时把这些预测以 `Advisor.PushPredictions` 推给 Manager，`ttl_ms = 10000`，调用超时 2 秒，失败不重试只记日志。

#### Scenario: AI 停止
- **WHEN** AI 服务停止
- **THEN** 10 秒内 Manager 所有节点的 `f_pred` 回到 1.0，`ai:pred` 过期消失，调度照常

### Requirement: 基线对比
AI 服务 SHALL 在线累计最近 1 小时 Holt 预测与朴素基线（当前值当预测）对 30 秒后实际值的平均绝对误差，经 `GET /api/ai/predictions` 和 `/api/ai/health` 暴露；并提供离线脚本 `experiments/predict_eval.py` 用 `node:{id}:ts` 的历史数据比较两者。上线前 Holt 的 MAE SHALL 不高于朴素基线，否则关闭 `predict.push`。

#### Scenario: 查看对比
- **WHEN** 请求 `GET /api/ai/predictions`
- **THEN** 返回每台节点最新预测、因子、Holt 与基线的 1 小时 MAE 和样本点数
