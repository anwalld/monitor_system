# Spec Delta

## Purpose

把每台节点的最新状态和最近一小时的每秒样本保存在 Redis，供看板、AI 服务和排查读取；热存储在调度链路的旁路上，故障时不影响上报处理。

## ADDED Requirements

### Requirement: 节点最新状态
每收到一条样本，Manager SHALL 更新 Hash `node:{id}`，至少包含字段 `ts_ms`、`seq`、`addr`、`ncpu`、`cpu_busy`、`mem_avail_ratio`、`disk_util`、`last_seen_ms`。

#### Scenario: 查看最新值
- **WHEN** w1 上报后 2 秒内执行 `HGETALL node:w1`
- **THEN** 返回的 `seq` 等于 w1 最近一次被接受的样本 seq，`cpu_busy` 等于该样本的值

### Requirement: 每秒样本时间序列
每条被接受的样本 SHALL 以压缩 JSON 写入 Sorted Set `node:{id}:ts`，score 为 `ts_ms`。Manager MUST 每分钟删除 score 早于当前时间 1 小时的成员。

#### Scenario: 时间序列增长
- **WHEN** w1 连续上报 10 秒
- **THEN** `ZCOUNT node:w1:ts <10 秒前> +inf` 返回 9 到 11

#### Scenario: 过期修剪
- **WHEN** Manager 运行超过 61 分钟
- **THEN** `node:w1:ts` 中不存在 score 早于 1 小时前的成员

### Requirement: 全节点样本流
Manager SHALL 每秒向 Stream `stream:metrics` 追加一条记录，包含这一秒内所有节点的最新样本，并以近似 MAXLEN 100000 限制长度。

#### Scenario: 多节点汇总
- **WHEN** 3 台 Worker 同时上报
- **THEN** `XREVRANGE stream:metrics + - COUNT 1` 返回的记录中包含 3 个节点的样本

### Requirement: 写入失败不影响调度
Redis 写入 MUST 由独立的存储线程批量完成（一次往返提交同一批写入）。写入失败 SHALL 只记录日志（限频），不重试积压数据；Redis 恢复后从下一批开始正常写入。

#### Scenario: Redis 恢复
- **WHEN** Redis 停止 30 秒后恢复
- **THEN** 恢复后 2 秒内 `node:{id}` 重新更新，Manager 进程内存不因停机期间的数据而持续增长
