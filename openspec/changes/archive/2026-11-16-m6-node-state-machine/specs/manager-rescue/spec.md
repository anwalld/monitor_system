# Spec Delta

## Purpose

当一台各项指标都不错的节点只因执行超时多而几乎选不中时，Manager 用高频探测确认它是否健康，健康就快速恢复它的权重。

## ADDED Requirements

### Requirement: 急救触发
正常状态的节点同时满足以下条件时 SHALL 进入急救：score ≥ 0.6；f_timeout < 1 且 f_timeout 不大于 f_inflight、f_slow、f_pred 中的任何一个；选中概率 p 小于 0.5 / N（N 为候选数）；当前时刻不早于退避截止时间。

#### Scenario: 触发
- **WHEN** w1、w2 都正常，score 分别为 0.9、0.65，w1 的 T 为 4（f_timeout ≈ 0.135），其余因子为 1
- **THEN** p ≈ 0.157 < 0.25，w1 进入急救

#### Scenario: 分数太低不急救
- **WHEN** 同上但 w1 的 score 为 0.5
- **THEN** w1 不进入急救

### Requirement: 急救退避
节点 SHALL 记录退避级别 level（每过 1 天减半）。进入急救时 SHALL 设置退避截止时间为当前时刻加 `min(300 s × 2^floor(level), 3600 s)`，然后 level 加 1。

#### Scenario: 指数退避
- **WHEN** w1 在 1 小时内连续满足急救条件
- **THEN** 第 1 次立即急救，之后间隔依次为 5、10、20、40、60、60 分钟

#### Scenario: 衰减
- **WHEN** w1 已急救 3 次（level 3），之后 2 天没有急救
- **THEN** level 衰减到 0.75，下一次急救后的退避间隔为 5 分钟

### Requirement: 急救双向流
Manager SHALL 为每个急救中的节点使用单独的 gRPC channel 打开 `WorkerTask.Rescue` 双向流，每 1 ms 发送一条 `RescueReq{seq（从 1 递增）, send_ts_us}`。待确认请求按先进先出排队：
- 回应 seq 等于队头：出队，计一次成功；
- 回应 seq 大于队头：其前面的请求按超时出队，该请求计成功；
- 回应 seq 小于队头：迟到，丢弃并计数；
- 队头请求发出超过 10 ms 仍无回应：出队，计一次超时。

一次超时发生时若不处于“中断”中，网络事件数 SHALL 加一并进入中断；任何一次成功 SHALL 退出中断。

#### Scenario: 连续超时算一次事件
- **WHEN** 第 1–100 条正常回应，第 101–300 条全部超时，第 301 条起恢复
- **THEN** 超时 200 次，网络事件 1 次

#### Scenario: 迟到丢弃
- **WHEN** 第 5 条超时出队后才收到它的回应
- **THEN** 该回应计为迟到，不计成功

### Requirement: 批结算与结束
Manager SHALL 每 100 ms 结算一批：本批没有超时为成功批，T SHALL 乘以 0.9，连续失败批数 k 清零；有超时为失败批，k 加一。目标 `target = max(0, 1/N − ε·(2ᵏ − 1))`，`ε = 0.125 / N`。选中概率 p（按节点回到候选计算）≥ target，或急救已满 10 s 时 SHALL 关闭流，并按节点新鲜度转为慢启动或剔除。

#### Scenario: 健康节点快速恢复
- **WHEN** 急救中的 w1 每批都成功
- **THEN** T 每批乘以 0.9，p 达到 1/N 时急救结束，w1 进入慢启动

#### Scenario: 持续失败
- **WHEN** 急救中的 w1 连续 4 批失败
- **THEN** target 依次为 0.4375、0.3125、0.0625、0（N = 2），第 4 批后结束急救

#### Scenario: 不可达
- **WHEN** 急救开始时 w1 的 50052 端口不可达
- **THEN** 急救在 1 秒内结束，日志中超时数大于 0
