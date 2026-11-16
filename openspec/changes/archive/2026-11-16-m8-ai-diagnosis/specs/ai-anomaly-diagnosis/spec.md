# Spec Delta

## Purpose

AI 服务用统计方法实时检测节点异常，用 LLM 把异常解释成结构化诊断；AI 只给建议，挂掉不影响调度。

## ADDED Requirements

### Requirement: 数据来源
AI 服务 SHALL 用 consumer group `ai` 消费 `stream:metrics` 和 `stream:events`，处理后 `XACK`；启动时 SHALL 从 `node:{id}:ts` 读最近 10 分钟样本预热检测窗口，并跳过 `stream:metrics` 里停机期间积压的条目。AI 服务 MUST NOT 写 Manager 使用的任何 Redis key（`node:*`、`stream:metrics` 的条目）；在 `stream:metrics` 上创建 consumer group（必要时 MKSTREAM 建出空 Stream）不算写入。

#### Scenario: 重启后立即可检测
- **WHEN** AI 服务重启
- **THEN** 不等 10 分钟，第一条新样本到来时检测窗口已有最近 10 分钟的数据

### Requirement: 统计检测
对每台节点的每个指标（CPU 忙碌度、调度延迟 P99、可用内存比例、磁盘利用率、每块网卡的 RTT、超时率、重传率），AI 服务 SHALL 保留最近 10 分钟窗口，计算稳健 z 分数 z = (x − 中位数) / (1.4826 × MAD)，只看变坏的方向（可用内存看下降，其他看上升），往变坏方向 z > 4 且连续 3 个点时判为异常。MAD 过小时 SHALL 用每个指标配置的最小尺度代替，避免常量序列上一点微小波动就报异常。窗口内样本少于 60 个时 SHALL NOT 做 z 分数检测。

#### Scenario: 持续高负载
- **WHEN** 一台空闲节点上运行 `stress -c 4`
- **THEN** CPU 越界后的第 3 个样本判为异常（每秒一个样本，实测从开始注入算 4–5 秒）

#### Scenario: 单点毛刺
- **WHEN** CPU 只有 1 到 2 个点突增后回落
- **THEN** 不判为异常

### Requirement: 事件触发的异常
下列情况 SHALL 直接判为异常：`stream:events` 里的 `evict`、`rescue_start`；样本里排队延迟丢弃率超过 1%；某块网卡超时率超过 20%。

#### Scenario: 节点被剔除
- **WHEN** Manager 写入一条 `evict` 事件
- **THEN** AI 服务对该节点产生一个严重度 high 的异常

### Requirement: 诊断去重
同一节点 10 分钟内 SHALL 只诊断一次；严重度高于该节点上一次诊断时除外。

#### Scenario: 异常持续
- **WHEN** 一台节点的 CPU 异常持续 5 分钟
- **THEN** 只产生一条诊断

### Requirement: 诊断流程
诊断 SHALL 异步进行，不阻塞检测。上下文 SHALL 包含异常本身、该节点最近 10 分钟每 10 秒一个点的指标、最近 30 分钟的状态转移和任务超时、同一时刻其他节点各指标的中位数。模型输出 SHALL 用 pydantic 按 schema（severity、summary、causes[cause, evidence, confidence]、actions）校验，不合格时带上错误重试一次，仍不合格则只保留统计结果。结果 SHALL 写入 `ai_diagnosis`，并向 `stream:events` 追加一条 `kind = diagnosis` 的事件，detail 至少含诊断 id、严重度、一句话结论和触发指标。诊断 MUST NOT 自动执行任何操作。

#### Scenario: 诊断上看板
- **WHEN** 检测到异常且 LLM 返回合格 JSON
- **THEN** `ai_diagnosis` 多一行，看板事件页实时出现一条 diagnosis 事件

### Requirement: LLM 调用约束
LLM 调用 SHALL 异步、30 秒超时、指数退避最多重试 2 次，并按 `ai:tokens:{日期}` 统计当日 token；达到每日预算后当天不再调用 LLM。没选厂商、所选厂商缺 Key 或超出预算时，诊断 SHALL 只含统计结果（代码生成的结论，`model` 为空）。发给模型的内容 MUST NOT 包含任务命令和输出；配置匿名时节点名 SHALL 替换为代号，返回后再换回。

#### Scenario: 未配置厂商
- **WHEN** `ai:settings` 里没有厂商
- **THEN** 检测和统计诊断照常写入，不发起任何 LLM 请求

#### Scenario: LLM 超时
- **WHEN** LLM 接口一直不响应
- **THEN** 3 次尝试后放弃，写入统计诊断，检测不中断

### Requirement: 故障隔离
AI 服务停止、卡住或 LLM 不可用时，Manager 的调度、上报和看板其他页面 SHALL 不受影响。Redis 或 MySQL 暂时不可用时 AI 服务 SHALL 不退出，恢复后继续。

#### Scenario: 停掉 AI 服务
- **WHEN** 压测期间 `docker compose stop ai`
- **THEN** 任务无失败，看板总览和事件页正常，只有诊断页和设置页报不可用
