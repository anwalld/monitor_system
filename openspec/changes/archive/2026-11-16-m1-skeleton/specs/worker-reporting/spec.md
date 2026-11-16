# Spec Delta

## Purpose

把 Worker 的采集与上报解耦：采集永远不被网络阻塞，上报永远只发最新的样本，Manager 不可达时 Worker 继续采集并在恢复后立即恢复上报。

## ADDED Requirements

### Requirement: 采集不受上报阻塞
采集与上报 SHALL 在不同线程运行，通过固定容量的无锁缓冲交换样本。上报变慢或 Manager 不可达时，采集周期 MUST 不受影响，缓冲满时覆盖最旧的未取样本。

#### Scenario: Manager 不可达
- **WHEN** Manager 停止运行 30 秒
- **THEN** 这 30 秒内 Worker 仍每秒生成样本，采集周期间隔仍在 900 到 1100 毫秒之间

### Requirement: 只上报最新样本
上报线程每个上报周期（默认 1 秒）SHALL 取缓冲中 `seq` 最大的样本发送；已发送过的样本 MUST NOT 再次发送，没有新样本时本周期不发送。

#### Scenario: 积压后恢复
- **WHEN** Manager 恢复可达
- **THEN** Worker 下一次上报的样本是当前最新样本，而不是停机期间积压的旧样本，Manager 收到的 `seq` 严格递增

### Requirement: 截止时间与失败处理
每次 Report MUST 设置截止时间（默认 500 毫秒）。失败或超时 SHALL 只记录日志，不重试、不排队重发；连续失败时日志 MUST 限频（每 10 秒最多一条汇总）。

#### Scenario: 调用超时
- **WHEN** Manager 处理 Report 超过 500 毫秒
- **THEN** Worker 在约 500 毫秒时放弃这次调用，下一秒发送更新的样本

### Requirement: 缓冲读写不产生撕裂数据
上报线程取到的样本 MUST 是采集线程完整写入的某一条样本，任何情况下都不得读到写了一半的数据，读到的 `seq` 不倒退。

#### Scenario: 高频并发
- **WHEN** 测试中生产者每 1 毫秒写一次、消费者随机延迟读取，持续 10 秒并开启 ThreadSanitizer
- **THEN** 消费者每次读到的样本内容自洽、`seq` 单调不减，ThreadSanitizer 无报告
