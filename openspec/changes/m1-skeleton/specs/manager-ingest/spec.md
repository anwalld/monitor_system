# Spec Delta

## Purpose

Manager 接收各 Worker 的上报，维护一张反映每台节点最新状态与新鲜度的节点表，作为后续评分、剔除和调度的唯一输入。

## ADDED Requirements

### Requirement: 首次上报登记新节点
Manager 收到一个未知 `node_id` 的样本时 SHALL 在节点表中登记该节点，记录节点地址（上报连接的对端 IP 加配置的 Worker 端口，默认 50052）、CPU 核数（`core_busy` 的长度）和首次出现时间。

#### Scenario: 新节点上报
- **WHEN** 从 10.0.0.11 上报的 `node_id=w1` 第一次到达
- **THEN** 节点表新增 w1，地址为 `10.0.0.11:50052`，核数等于样本中 `core_busy` 的元素个数

### Requirement: 更新最新样本与新鲜度
每收到一条样本，Manager SHALL 用它替换该节点的最新样本，并把最近上报时间更新为 Manager 收到时的本地时间。`seq` 不大于已记录 `seq` 的样本 MUST 被丢弃且仍返回 OK；Worker 重启导致 `seq` 回到较小值时，若距上次上报超过 3 秒则视为新进程并接受。

#### Scenario: 乱序样本
- **WHEN** w1 已记录 seq=100，又收到 seq=99 的样本
- **THEN** 节点表不变，Report 返回 OK

#### Scenario: Worker 重启
- **WHEN** w1 停止 5 秒后重启，发来 seq=1 的样本
- **THEN** 节点表接受该样本，最新 seq 变为 1

### Requirement: 处理上报不做 IO
Manager 处理 Report 时 MUST 只在内存中更新节点表并把待写数据交给存储线程，持有节点表锁期间不得进行任何网络或磁盘 IO；Redis 不可用时 Report 仍 SHALL 正常返回。

#### Scenario: Redis 停止
- **WHEN** Redis 停止后 Worker 继续上报
- **THEN** 所有 Report 仍在 50 毫秒内返回 OK，节点表持续更新

### Requirement: 节点新鲜度可查询
Manager SHALL 能给出每个节点距最近一次上报经过的时间，并在日志中每 10 秒输出一次在线节点数（3 秒内有上报）和全部节点数。

#### Scenario: 节点停止上报
- **WHEN** w1 停止上报 5 秒
- **THEN** 下一次汇总日志中在线节点数比全部节点数少 1
