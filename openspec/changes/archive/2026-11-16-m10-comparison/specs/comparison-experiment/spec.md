# comparison-experiment Specification

## Purpose
用固定的任务序列和干扰序列，对比轮询、加权轮询、本系统不带预测、本系统带预测四组的任务完成时间与反应速度。

## ADDED Requirements

### Requirement: 工作负载
实验 SHALL 混合 CPU 密集、IO 密集、网络密集三类任务，任务序列由运行种子决定（`lm-bench --workload`）。同一轮内四组 SHALL 使用同一种子；不同轮 SHALL 使用不同种子。

#### Scenario: 同轮同序列
- **WHEN** 第 3 轮的四组运行结束
- **THEN** 四份明细的任务数、到达时刻、类型序列相同

### Requirement: 干扰注入
每次运行 SHALL 注入一次本地 CPU 负载（在随机节点上运行 `stress -c <核数>`），开启 `--net-fault` 时另注入一次网卡劣化（在随机节点的数据面网卡上加 netem 延迟与丢包）；节点、开始时刻、持续时间由运行种子决定，同一轮四组相同。注入与撤销的实际时刻（相对压测开始的秒数）SHALL 记录到运行目录。运行结束或脚本中断时 SHALL 撤销所有注入；虚拟机上改流量控制前 SHALL 设好自动撤销，MUST NOT 改 ssh 所用的网卡。

#### Scenario: 中断清理
- **WHEN** 运行中按 Ctrl-C
- **THEN** stress 进程被结束，netem 规则被删除

### Requirement: 交替运行
脚本 SHALL 按轮运行，第 r 轮的组顺序为基础顺序左移 r 位；每次运行前 SHALL 等待至少 35 秒，并等所有节点都不处于急救状态（最多再等 120 秒）。每次运行的明细 CSV、注入记录、开始前的节点状态 SHALL 存在 `<输出目录>/r<轮>-<组>/`。

#### Scenario: 顺序轮换
- **WHEN** 基础顺序为 rr、wrr、score_nopred、score，运行第 1 轮（从 0 计）
- **THEN** 该轮顺序为 wrr、score_nopred、score、rr

### Requirement: 指标与统计
统计脚本 SHALL 对每次运行计算：成功任务完成时间 P50、P99、最大值，吞吐（成功任务数 / 运行秒数），失败数（按阶段分），以及 CPU 干扰节点的反应时间（干扰前 30 秒占比为基线，5 秒窗口占比首次 ≤ 基线一半的时刻减去干扰开始时刻；60 秒内未降记“未降”；基线 < 0.1 或窗口分配数 < 10 时不计）。每组 SHALL 汇总各指标的均值与标准差，并给出各组相对轮询组的 P50、P99 差值、Cohen's d 与 Welch t 检验 p 值（正态近似）；按任务类型另列 P99。

#### Scenario: 轮询不反应
- **WHEN** 轮询组的 CPU 干扰节点占比始终约为 1/N
- **THEN** 该次运行的反应时间记为“未降”
