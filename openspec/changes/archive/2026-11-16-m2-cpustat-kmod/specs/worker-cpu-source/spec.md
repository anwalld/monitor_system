# Spec Delta

## Purpose

Worker 选择 CPU 读数来源：优先从 cpustat 共享页做一致性读取，缺少模块时回退到 /proc/stat。读取开销降到足以支撑 1 毫秒采样，上报的样本语义不变。

## ADDED Requirements

### Requirement: 来源选择与回退
Worker SHALL 支持配置项 `cpu_source`，取值为 `auto`、`shm`、`proc`，默认 `auto`：
- `auto`：`/dev/cpustat` 能打开并映射时用共享页，否则用 `/proc/stat`，并记一条警告说明原因。
- `shm`：打不开或映射失败时，Worker MUST 以非 0 退出码退出并记录错误。
- `proc`：始终用 `/proc/stat`。

非法取值 MUST 让 Worker 启动失败，错误信息中包含 `cpu_source`。启动日志 SHALL 写明实际使用的来源。

#### Scenario: 模块已加载
- **WHEN** `cpustat` 模块已加载、`/dev/cpustat` 对 `lm` 组可读，Worker 以默认配置启动
- **THEN** 启动日志写明 CPU 来源为共享页，样本照常每秒生成

#### Scenario: 模块未加载
- **WHEN** 没有 `/dev/cpustat`，Worker 以 `cpu_source: auto` 启动
- **THEN** 日志中有一条警告说明回退到 /proc/stat，样本照常每秒生成

#### Scenario: 强制共享页但模块缺失
- **WHEN** 没有 `/dev/cpustat`，Worker 以 `cpu_source: shm` 启动
- **THEN** Worker 记录错误并以非 0 退出码退出

### Requirement: seqlock 一致性读取
读取共享页时 SHALL 按 seq 协议进行：读前 `seq` 为奇数时重试；拷贝 `ncpu` 条记录后再读一次 `seq`，与读前相同才接受这次快照。单次读取最多重试 100 次，仍不一致时本次读取 MUST 失败。失败按采集失败处理：沿用上一次的值、记警告，不中断采集。`ncpu` 大于 256 时 MUST 按 256 截断。

#### Scenario: 写者并发
- **WHEN** 一个线程按奇偶 seq 协议不停地把共享页前一半和后一半写成同一个递增值，4 个读线程各读取 10 万次
- **THEN** 每次成功读到的快照，前后两半的值都相同；没有读到奇数 seq 的快照

#### Scenario: 写者一直卡在奇数
- **WHEN** `seq` 停在奇数不再变化
- **THEN** 读取在 100 次重试后返回失败，不阻塞调用方

### Requirement: 忙碌度时间基准
使用共享页时，每核的时间差 SHALL 取该核 `stamp_us` 的差值，空闲差取 `idle_us + iowait_us` 的差值，不使用名义刷新周期。上报的 `cpu_busy`、`core_busy` 语义与 M1 一致，公式按 `worker-metric-collection` 的规定。两次读到同一次刷新（各核 `stamp_us` 都没变）时，本次 MUST 沿用上一次的忙碌度生成样本，且不更新基线，下一次按更长的时间差计算。

#### Scenario: 单核打满（共享页来源）
- **WHEN** 模块已加载，在 4 核空闲主机上用 `taskset -c 2 stress -c 1` 打满 CPU 2
- **THEN** 样本中 `core_busy[2]` 不低于 0.95，`cpu_busy` 在 0.2 到 0.35 之间

#### Scenario: 读到同一次刷新
- **WHEN** 相邻两次采集读到的各核 `stamp_us` 完全相同
- **THEN** 这次仍生成样本，`cpu_busy`、`core_busy` 与上一条样本相同；再下一次的时间差从上一个基线算起

#### Scenario: 两种来源一致
- **WHEN** 同一台主机上分别以 `cpu_source: shm` 和 `cpu_source: proc` 运行 `--dry-run` 各 30 秒，期间保持 `stress -c 2` 负载
- **THEN** 两组样本的 `cpu_busy` 平均值相差不超过 0.05

### Requirement: 读取开销
从共享页读取一次快照的耗时 SHALL 比从 `/proc/stat` 读取低 85% 以上。对照组 `/proc/stat` 用常开文件描述符加 `pread`、只解析 `cpuN` 行。测量方法：绑定到一个核，预热 1000 次后测 10 万次，用 CLOCK_MONOTONIC 计时并扣除计时本身的开销，比较 P50。计时器一跳比单次读取还长时（如 arm64 虚拟机约 41 ns），按每 100 次读取一批计时、除以 100 作为单次耗时，两种来源用同一方法。

#### Scenario: 基准对比
- **WHEN** 在 4 核 Worker 虚拟机上运行读取耗时基准程序
- **THEN** 输出两种来源的 P50 与 P99，共享页的 P50 不超过 /proc/stat 的 15%

### Requirement: 刷新周期守卫
Worker SHALL 提供作用域守卫：进入作用域时记下当前刷新周期并设为指定值（急救用 1000 微秒），离开作用域时恢复为进入前的周期，包括因异常离开的情况。设置失败时 MUST 记录警告并继续运行，不影响采集。来源不是共享页时，守卫什么也不做。

#### Scenario: 正常进出
- **WHEN** 周期为默认 1 秒时，守卫以 1000 微秒构造，随后析构
- **THEN** 构造后共享页 `interval_us` 为 1000，析构后恢复为 1000000

#### Scenario: 异常离开
- **WHEN** 守卫所在作用域抛出异常
- **THEN** 共享页 `interval_us` 仍恢复为 1000000
