# Spec Delta

## Purpose

Worker 用 eBPF 测量本机任务从“可以运行”到“真正上 CPU”的排队延迟，每秒给出 P50、P99 和丢弃率并随样本上报。这是 CPU 忙碌度之外判断节点是否拥挤的指标。

## ADDED Requirements

### Requirement: 测量口径
一次排队延迟 SHALL 从任务进入可运行状态开始，到它被切换上 CPU 结束。进入可运行状态包括三种情况：被唤醒（`sched_wakeup`）、新建后首次唤醒（`sched_wakeup_new`）、在 `sched_switch` 中被换下但仍处于可运行状态（被抢占或主动让出后重新排队）。pid 为 0 的 idle 线程 MUST 排除。统计范围是整台主机的所有任务，不区分核和进程。

#### Scenario: 被抢占的任务重新计时
- **WHEN** 一个任务在 `sched_switch` 中被换下，且换下时状态为 TASK_RUNNING
- **THEN** 从这一刻起重新开始计它的排队时间，下次它被换上 CPU 时记一次延迟

#### Scenario: 没有入队记录的任务
- **WHEN** 一个任务被换上 CPU，但之前没有它的入队时间（例如程序加载前就已在排队）
- **THEN** 这次不计入直方图

### Requirement: 对数线性分桶
延迟以微秒为单位记入 208 个桶。值 v 小于 8 时落在第 v 桶。v 不小于 8 时，设 k = ⌊log₂ v⌋，桶号为 (k − 2) × 8 + ((v >> (k − 3)) & 7)，即每个 2 的幂区间再均分 8 份。桶号超过 207 时 MUST 记入第 207 桶（对应约 268 秒以上）。内核程序与用户态 MUST 使用同一套分桶公式。

#### Scenario: 桶边界
- **WHEN** 分别对 0、7、8、15、16、100、1000 微秒求桶号
- **THEN** 依次得到 0、7、8、15、16、36、63

#### Scenario: 超大值
- **WHEN** 对 2³⁰ 微秒求桶号
- **THEN** 得到 207

### Requirement: 每周期分位数
Worker SHALL 在每次采集时把各核直方图按桶求和，与上一次采集的累计值相减，得到本周期的直方图，再按以下方法计算 P50、P99：
- 目标秩 r = p × N，N 为本周期样本总数。
- 找到累计计数首次不小于 r 的桶，在该桶区间 [下界, 下界 + 宽度) 内按比例线性插值。

本周期样本数为 0 时，P50、P99 SHALL 为 0。进程启动后的第一次采集 MUST 只建立基线，不填这三个字段。

#### Scenario: 桶内插值
- **WHEN** 本周期只有第 16 桶（区间 [16, 18) 微秒）有 100 个样本
- **THEN** P50 为 17.0 微秒，P99 为 17.98 微秒

#### Scenario: 周期差分
- **WHEN** 上一次累计第 8 桶为 1000，这一次为 1010，其他桶不变
- **THEN** 本周期 N = 10，分位数只由这 10 个样本决定

#### Scenario: 空闲周期
- **WHEN** 本周期没有任何调度事件
- **THEN** 样本中 `runq_p50_us`、`runq_p99_us`、`runq_drop_rate` 都为 0，并且都被设置（区别于未上报）

### Requirement: 丢弃率
入队时间表满（10240 项）导致无法记录时，内核程序 SHALL 增加丢弃计数。每周期丢弃率 SHALL 为 Δ丢弃 / (Δ丢弃 + Δ样本数)，分母为 0 时为 0，结果截断到 [0, 1]。

#### Scenario: 计算丢弃率
- **WHEN** 本周期 Δ丢弃 = 10，Δ样本数 = 990
- **THEN** `runq_drop_rate` 为 0.01

### Requirement: 启用与降级
Worker SHALL 支持配置项 `runq`，取值为 `auto`、`on`、`off`，默认 `auto`：
- `auto`：加载或挂载 eBPF 程序失败时记一条警告（包含原因），样本不设置三个调度延迟字段，其余指标照常采集。
- `on`：加载失败时 MUST 以非 0 退出码退出。
- `off`：不加载，不设置这三个字段。

非法取值 MUST 让 Worker 启动失败，错误信息中包含 `runq`。启动日志 SHALL 写明调度延迟采集是否启用。构建时没有 eBPF 支持（如 macOS）的 Worker，按“加载失败”处理。运行中读取 map 失败时，按采集失败处理：沿用上一次的值并记警告。

#### Scenario: 以 lm 用户在 systemd 下运行
- **WHEN** Worker 以 systemd 单元（用户 lm，环境能力 CAP_BPF、CAP_PERFMON）在 Ubuntu 24.04 上启动
- **THEN** 日志写明调度延迟采集已启用，输出的样本都设置了三个字段（首次采集同时建立 CPU 与调度延迟基线，不出样本）

#### Scenario: 缺少权限
- **WHEN** 以没有 CAP_BPF 的普通用户运行 Worker，`runq: auto`
- **THEN** 日志中有一条警告说明加载失败的原因，样本不设置三个调度延迟字段，CPU、内存、磁盘照常上报

#### Scenario: 强制开启但失败
- **WHEN** 同样没有权限，`runq: on`
- **THEN** Worker 记录错误并以非 0 退出码退出

### Requirement: 反映 CPU 拥挤并与 bcc runqlat 一致
调度延迟 SHALL 能反映核上的排队情况，分布 SHALL 与 bcc `runqlat` 接近。比较方法：两者在同一负载下同时运行 30 秒；把本系统的直方图按 2 的幂重新分桶，与 `runqlat` 的输出各自归一化后比较。

#### Scenario: 单核挤 8 个线程
- **WHEN** 在 4 核主机上用 `taskset -c 1 stress -c 8` 把 8 个线程绑到同一个核
- **THEN** 样本中 `runq_p99_us` 不低于 1000，并且不低于空闲时 P99 的 10 倍

#### Scenario: 与 runqlat 对照
- **WHEN** 在上一场景的负载下，对照工具与 `runqlat-bpfcc 30 1` 同时运行 30 秒
- **THEN** 两份按 2 的幂分桶的归一化直方图，总变差距离（各桶比例差的绝对值之和的一半）不超过 0.1；按各自直方图估算的 P50、P99 相差不超过 2 倍

### Requirement: 采集开销
启用调度延迟采集后，空闲主机上 Worker 进程的 CPU 占用 SHALL 不超过单核的 2%。每次采集读取并合并直方图的耗时 SHALL 不超过 1 毫秒（4 核主机）。

#### Scenario: 空闲开销
- **WHEN** Worker 在空闲的 4 核虚拟机上启用调度延迟采集运行 60 秒
- **THEN** 这段时间里 Worker 进程的 CPU 时间不超过 1.2 秒
