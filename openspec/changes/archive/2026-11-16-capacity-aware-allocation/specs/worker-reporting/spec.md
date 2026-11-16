# Spec Delta

## MODIFIED Requirements

### Requirement: 只上报最新样本
上报线程每个上报周期（默认 1 秒）SHALL 取缓冲中 `seq` 最大的样本发送；已发送过的样本 MUST NOT 再次发送，没有新样本时本周期不发送。发送前 SHALL 把任务服务实际生效的并发上限填入 `max_concurrent`（任务服务关闭时为 0）。

#### Scenario: 积压后恢复
- **WHEN** Manager 恢复可达
- **THEN** Worker 下一次上报的样本是当前最新样本，而不是停机期间积压的旧样本，Manager 收到的 `seq` 严格递增
