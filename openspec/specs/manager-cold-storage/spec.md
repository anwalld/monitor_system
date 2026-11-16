# manager-cold-storage Specification

## Purpose
Manager 把节点登记、5 分钟聚合指标、事件和任务记录写入 MySQL，供历史回看、日报和对照实验使用。MySQL 的写入不影响调度和 Redis 热数据。

## Requirements

### Requirement: 独立的 MySQL 写线程
MySQL 写入 SHALL 在独立线程中进行，通过容量 16384 的队列接收数据，队列满时丢弃最旧的项并计数。MySQL 不可用时 SHALL 只记日志（限频），每 5 秒最多尝试重连一次；调度、Allocate 和 Redis 写入 MUST NOT 受影响。密码 SHALL 从环境变量 `LM_MYSQL_PASSWORD` 读取。

#### Scenario: MySQL 停止
- **WHEN** MySQL 容器停止 60 秒后恢复
- **THEN** 期间 Allocate 正常、`node:{id}` 持续更新；恢复后新的聚合行和任务行继续写入

### Requirement: node 表
新节点首次被接受时 SHALL 插入 `node`（node_id、addr、ncpu、first_seen）；已存在的节点地址或核数变化时 SHALL 更新 addr、ncpu，不改 first_seen。

#### Scenario: 登记
- **WHEN** w1 第一次上报，地址 10.0.0.1:50052、4 核
- **THEN** `node` 表有一行 w1、10.0.0.1:50052、4

### Requirement: 5 分钟聚合
Manager SHALL 按节点、按墙上时间对齐的 5 分钟桶（`floor(ts/300000)×300000`）累计被接受的样本，桶结束 5 秒后写入 `node_metrics_5m` 一行：`cpu_avg`、`cpu_max`、`runq_p99_max_us`、`mem_avail_min`、`net_rtt_avg_ms`、`net_timeout_rate`、`net_retrans_rate`（各网卡平均后再在桶内平均）、`score_avg`、`samples`。桶内没有调度延迟或网卡数据的列 SHALL 写 NULL。正常退出时 SHALL 写出未结束的桶。重复写同一桶 SHALL 覆盖。

#### Scenario: 每 5 分钟一行
- **WHEN** w1 每秒上报，持续 11 分钟
- **THEN** `node_metrics_5m` 中 w1 至少有 2 行完整的桶，`samples` 约为 300，`bucket_start` 相差 5 分钟

#### Scenario: 聚合口径
- **WHEN** 一个桶内 w1 的 CPU 为 0.2、0.4、0.6，P99 为 100、300、200 µs，可用内存为 0.5、0.3、0.4
- **THEN** 该行 cpu_avg 0.4、cpu_max 0.6、runq_p99_max_us 300、mem_avail_min 0.3、samples 3

### Requirement: node_event 表
每条事件 SHALL 写入 `node_event`（node_id、毫秒精度 ts、kind、detail），detail 中 SHALL 附带 `event_id`（`stream:events` 的 entry id，Redis 不可用时为空串）。

#### Scenario: 事件落库
- **WHEN** w1 被剔除
- **THEN** `node_event` 中有一行 w1、`evict`，detail 的 `event_id` 与 `stream:events` 中对应 entry 的 id 相同

### Requirement: task 表
在途账的每一步 SHALL 生成任务变化：分配（`allocated`，带 node_id、input_bytes、t_alloc）、MarkSent（`sent`、t_sent）、RECEIVED（`received`、t_recv）、DONE/FAILED（`done`/`failed`、t_done，迟到的也要）、确认超时未确认与执行超时（`timeout`）、作废（`void`）。同一任务 1 秒内的变化 SHALL 合并后批量写入：时间列取第一次的值，状态取等级较高者（allocated < sent < received < 终态）；终态 `timeout` 之后到达的 `done`/`failed` SHALL 覆盖它。

#### Scenario: 正常任务
- **WHEN** 一个任务依次 Allocate、MarkSent、RECEIVED、DONE
- **THEN** `task` 表一行，state 为 `done`，四个时间列都有值且递增

#### Scenario: 迟到的完成
- **WHEN** 任务执行超时后收到 DONE
- **THEN** `task` 表中该任务 state 从 `timeout` 变为 `done`，t_done 有值

#### Scenario: 作废
- **WHEN** 任务分配后 30 秒没有 MarkSent 和事件
- **THEN** 该任务 state 为 `void`
