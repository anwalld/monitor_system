# manager-hot-storage Specification

## Purpose
把每台节点的最新状态和最近一小时的每秒样本保存在 Redis，供看板、AI 服务和排查读取；热存储在调度链路的旁路上，故障时不影响上报处理。

## Requirements

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

### Requirement: 节点 Hash 中的调度延迟字段
样本设置了 `runq_p50_us`、`runq_p99_us`、`runq_drop_rate` 时，Manager SHALL 把它们以同名字段写入 Hash `node:{id}`，与其他字段在同一次更新中写入。样本没有设置这三个字段时，Manager MUST 在同一次更新中删除 Hash 中已有的这三个字段，避免读方看到过时的值。`node:{id}:ts` 与 `stream:metrics` 中的样本 JSON SHALL 在字段被设置时包含它们（包括值为 0 的情况），未设置时不包含。

#### Scenario: 新版 Worker 上报
- **WHEN** w1 上报的样本设置了 `runq_p99_us = 850`
- **THEN** `HGET node:w1 runq_p99_us` 返回 850，`node:w1:ts` 最新成员的 JSON 中有 `"runq_p99_us":850`

#### Scenario: 值为 0 仍写入
- **WHEN** 样本设置了 `runq_drop_rate = 0`
- **THEN** `HGET node:w1 runq_drop_rate` 返回 0，样本 JSON 中包含 `runq_drop_rate`

#### Scenario: 未上报调度延迟
- **WHEN** w2 的样本没有设置三个调度延迟字段
- **THEN** `HEXISTS node:w2 runq_p99_us` 为 0，样本 JSON 中不含这三个字段

#### Scenario: 关闭调度延迟后旧值被清除
- **WHEN** w1 先上报了带 `runq_p99_us` 的样本，之后以 `runq: off` 重启并继续上报
- **THEN** 重启后第一条样本写入后，`HEXISTS node:w1 runq_p99_us` 为 0

### Requirement: 节点 Hash 中的分数明细与剔除状态
每次接受上报后写 `node:{id}` 时，Manager SHALL 在同一个 HSET 中写入：`score`、`s_cpu`、`s_runq`、`s_mem`、`s_net`（小数点后 4 位，缺失项写实际计入的分数）、`best_nic`（没有网卡时为空串）、`evicted`（`0` 或 `1`）、`evict_reason`（剔除的指标名，多个用逗号连接，未剔除时为空串）。原有字段 MUST 保持不变。

#### Scenario: 正常节点
- **WHEN** 节点 w1 上报一条各项满分、网卡 ens160 的样本
- **THEN** `HGETALL node:w1` 中 `score` 为 `1.0000`，`best_nic` 为 `ens160`，`evicted` 为 `0`，`evict_reason` 为空

#### Scenario: 剔除节点
- **WHEN** w1 的 CPU 忙碌度连续 3 次为 0.98
- **THEN** 第 3 次之后 `node:w1` 中 `evicted` 为 `1`，`evict_reason` 为 `cpu`

### Requirement: 节点 Hash 中的选中概率
Manager SHALL 每 100 ms 计算每台节点的选中概率（候选节点为其权重占总权重的比例，非候选为 0），并在写 `node:{id}` 时写入 `prob`（小数点后 4 位）。

#### Scenario: 两台节点
- **WHEN** w1、w2 都正常，权重分别为 0.6、0.4
- **THEN** `node:w1` 的 `prob` 为 `0.6000`，`node:w2` 的为 `0.4000`

### Requirement: 节点 Hash 中的预测因子
`node:{id}` Hash SHALL 包含 `f_pred`（当前生效的预测因子，无预测或过期时为 1）；有未过期预测时包含 `pred_cpu`（预测的 30 秒后 CPU 忙碌度），否则删除该字段。

#### Scenario: 预测过期
- **WHEN** 最近一次预测已过期后节点再次上报
- **THEN** Hash 里 `f_pred = 1`，没有 `pred_cpu`
