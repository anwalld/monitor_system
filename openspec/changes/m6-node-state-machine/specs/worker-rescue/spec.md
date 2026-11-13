# Spec Delta

## Purpose

Worker 配合 Manager 的急救：在急救期间把内核 CPU 统计刷新周期切到 1 ms，并对每条急救请求回一份新样本。

## ADDED Requirements

### Requirement: Rescue 流
任务服务开启时，`WorkerTask.Rescue` SHALL：
- 同一时刻只服务一条流；已有流时新的流返回 `ABORTED`。
- 收到第一条请求时，若 CPU 来源是 cpustat 共享页，把刷新周期改为 1000 µs。
- 对每条请求回一条 `RescueResp{seq, send_ts_us（原样）, sample}`；sample 的 CPU 忙碌度为本次重新读取 CPU 计算的值（无新数据时沿用上次），`ts_ms` 为当前时间，其余字段取采集线程的最新样本。
- 流结束时把刷新周期恢复为进入前的值。
- 流持续超过 `task.rescue_max_s`（默认 15 s）时 SHALL 结束流、恢复周期并返回 `DEADLINE_EXCEEDED`。
- 不校验 token。

#### Scenario: 周期切换与恢复
- **WHEN** Manager 打开 Rescue 流发送 1000 条请求后关闭
- **THEN** 流期间 `/dev/cpustat` 的刷新周期为 1000 µs，收到 1000 条 seq 一致的回应；关闭后周期恢复为 1000000 µs

#### Scenario: 第二条流
- **WHEN** 一条急救流进行中又打开第二条
- **THEN** 第二条返回 `ABORTED`，第一条不受影响

#### Scenario: 时长上限
- **WHEN** `rescue_max_s` 为 1，客户端持续发送 2 秒
- **THEN** 约 1 秒时流以 `DEADLINE_EXCEEDED` 结束，周期已恢复
