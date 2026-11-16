# Proposal

## Why

对应开发文档里程碑 M10（对照实验）：证明本系统的调度比轮询、按核数加权轮询好，并量化负载预测的作用。验收：得到四组的完成时间 P50、P99 对比，以及本地负载出现后节点任务占比降到一半的反应时间。

现在只有本系统一种分配方式，`lm-bench` 只能跑单一命令，也没有中途注入干扰、交替跑组、统计多轮结果的工具。

开发文档要求在十几台 Worker 上做。用户 2026-11-16 决定先用现有的虚拟机和 Docker 完成全流程和一次完整的四组 × 10 轮实验，加机器以后再说：节点是虚拟机 lm-worker-1 和 Mac 上两个限 2 核的测试容器（lmtest-worker2、lmtest-worker3）。所以本变更交付的是“可在任意节点数上重复的实验工具 + 三节点的实测结果”，结论注明节点数限制。

## What Changes

- **对照分配策略**（proto、manager）：`AllocateReq` 新增 `policy` 字段（字段号 4）。取值：
  - 空或 `score`：本系统（带预测），与现在完全一样；
  - `score_nopred`：本系统，但 `f_pred` 恒取 1；
  - `rr`：轮询；
  - `wrr`：按核数加权的平滑轮询（`ncpu` 为权重）。
  - 对照组的候选集合只做最基本的健康检查：最近 3 秒有上报、低频探测未连续失败到剔除线。不看分数、关键指标剔除、急救、慢启动和各修正因子，模拟常见负载均衡器的做法。
  - 四种策略都走同一套在途账、任务记录和事件流程，只换“选哪台”，保证控制变量。`manager.yaml` 新增 `allocation.allow_policy`（默认 true），关掉后非默认策略返回 `FAILED_PRECONDITION`。
- **压测客户端**（experiments）：`lm-bench submit` 新增 `--policy`、`--workload`（任务类型文件：名称、比例、命令、参数，按独立的固定种子随机抽类型，到达序列不变）、`--host-map`（按 Worker 地址替换参数里的 `{host}`，给网络型任务指定下载源）；CSV 末尾加 `kind` 列；汇总按类型分别给分位数。
- **实验工具**（experiments）：
  - `compare_workload.txt`：CPU 密集（固定次数的 shell 循环）、IO 密集（`dd oflag=direct` 写 16 MB）、网络密集（从 Mac 上的数据源下载 4 MB）三类，比例 2:1:1。
  - `compare_data_server.py`：给网络型任务提供下载源（Mac 上只监听指定地址）。
  - `compare.py`：按轮交替跑各组（每轮换顺序），每次运行用同一种子的任务序列；按种子随机挑节点、随机时刻注入本地 CPU 负载（`stress`），可选再注入网卡劣化（`tc netem`，默认关，见 design 决定 7），记录注入时刻；每次运行前等节点恢复。
  - `compare_stats.py`：每次运行的 P50、P99、最大值、吞吐、超时和失败数，CPU 注入节点占比降到一半的时间；按组汇总均值与标准差，判断组间差是否明显大于组内波动。
- **Worker 的 cgroup CPU 来源**（worker）：`cpu_source: cgroup` 按 cgroup v2 的使用量和配额计算忙碌度。测试容器 lmtest-worker2 原来读整台 Docker 虚拟机的 `/proc`，容器里的 stress 低优先级跑时既拖不慢任务、又让指标虚高；改成限 2 核 + cgroup 来源后，干扰和指标都只在这 2 核里，核数也能真实上报给加权轮询。
- **AI**（不改代码）：实验期间用打开 `predict.push` 的临时镜像，实验结束换回关闭推送的正式镜像。

## Capabilities

### New Capabilities
- `comparison-experiment`：工作负载、干扰注入、交替运行、指标与统计口径。

### Modified Capabilities
- `manager-allocation`：`Scheduler.Allocate` 支持 `policy`；新增对照策略的候选集合与选法。
- `bench-client`：`submit` 新增 `--policy`、`--workload`、`--host-map`；输出加任务类型。
- `worker-cpu-source`：新增 `cpu_source: cgroup`。

## Impact

- 对应里程碑：M10。影响模块：proto（`task.proto` 加一个字段）、manager（分配）、worker（cgroup CPU 来源）、config、experiments、deploy（不改 compose）。
- **兼容性**：`AllocateReq.policy` 是新字段号 4，老客户端不填即默认策略，行为不变；老 Manager 会忽略该字段（按本系统分配），实验脚本会先用 `lm-bench allocate --policy rr` 检查 Manager 是否支持。Redis key、MySQL 表不变。
- **不在本次范围**：
  - 十几台 Worker 上的正式实验（等加机器后用同一套工具重跑）；M0 4.2、M1 7.1、M5b 5.4、M6 4.3/4.4 这些要真机的验收。
  - 按实验结果调默认参数（开发文档说跑完对照实验再调）：本变更只记录结果和建议，不改 `scoring` 默认值。
  - iperf 型网络干扰：节点之间没有可打满的共享链路，用 netem 劣化代替。
