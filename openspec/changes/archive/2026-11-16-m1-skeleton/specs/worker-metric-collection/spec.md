# Spec Delta

## Purpose

Worker 每秒采集一次本节点的 CPU、内存、磁盘指标并形成样本，为上报和调度提供节点负载的真实反映，包括他人直接登录运行的负载。

## ADDED Requirements

### Requirement: 每秒采集一次
Worker SHALL 以配置的采集周期（默认 1 秒）采集一次并生成一条样本，`ts_ms` 为本次采集时刻。

#### Scenario: 稳定周期
- **WHEN** Worker 连续运行 60 秒
- **THEN** 生成的样本数在 59 到 61 之间，相邻样本 `ts_ms` 间隔在 900 到 1100 毫秒之间

### Requirement: CPU 忙碌度按真实时间差计算
每核忙碌度 SHALL 由相邻两次读数计算：`busy = clamp((Δtotal − Δ(idle+iowait)) / Δtotal, 0, 1)`；全机忙碌度为各核平均。进程启动后的第一次读数 MUST 只作为基线，不产生忙碌度。

#### Scenario: 首次采集
- **WHEN** Worker 刚启动完成第一次采集
- **THEN** 这一次不生成样本，从第二次采集开始生成

#### Scenario: 单核打满
- **WHEN** 在 4 核空闲主机上用 `taskset -c 2 stress -c 1` 打满 CPU 2
- **THEN** 样本中 `core_busy[2]` 不低于 0.95，`cpu_busy` 在 0.2 到 0.35 之间

#### Scenario: 他人登录产生的负载
- **WHEN** 不通过调度系统、直接登录 Worker 主机运行 `stress -c <核数>`
- **THEN** 随后的样本中 `cpu_busy` 不低于 0.9

### Requirement: 内存与磁盘指标
`mem_avail_ratio` SHALL 等于 MemAvailable / MemTotal。`disk_util` SHALL 由块设备累计忙碌毫秒数的差值除以经过的毫秒数得到，取所有物理磁盘中的最大值，截断到 [0, 1]；分区、loop、ram 设备 MUST 排除。

#### Scenario: 磁盘繁忙
- **WHEN** 对某块物理磁盘持续执行 `fio --rw=randread --direct=1`
- **THEN** 样本中 `disk_util` 明显高于空闲时（不低于 0.3），且与同期按 `/proc/diskstats` io_ticks 计算的该盘利用率（等同 `iostat` 的 %util）误差不超过 0.05

#### Scenario: 内存占用上升
- **WHEN** 用 `stress --vm 1 --vm-bytes <约一半内存> --vm-keep` 占用内存
- **THEN** `mem_avail_ratio` 比占用前下降约 0.5（误差 0.1 以内）

### Requirement: 采集失败不中断
某一项指标读取失败时，Worker MUST 记录日志并继续生成样本，该项沿用上一次的值；连续失败超过 10 次时 SHALL 以错误级别记录。

#### Scenario: 单项读取失败
- **WHEN** 磁盘统计读取失败一次
- **THEN** 该周期仍生成样本，`disk_util` 等于上一周期的值，日志中有一条警告
