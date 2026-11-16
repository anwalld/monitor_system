# Proposal

## Why

M10 对照实验（2026-11-16，三节点）发现：本系统各组每次运行约 100–115 个任务失败，是轮询的约 3 倍，几乎全是 Worker 并发上限满了拒收（`Submit` 返回 `RESOURCE_EXHAUSTED`）。一台节点被长期占用、一台被干扰剔除时，本系统把任务集中到剩下的 2 核容器上，而评分不知道 Worker 的并发上限；`f_inflight` 只在在途量超过自身基线时缓慢降权，挡不住短时间的集中。

属于 M10 之后的调度修正（不是新里程碑），影响模块：proto、worker、manager、experiments（复跑对照）。

## What Changes

- **proto**：`Sample` 追加 `uint32 max_concurrent = 12`：Worker 任务服务的并发上限，0 表示未开任务服务或不限制。
- **worker**：任务服务开启时，每条上报带上实际生效的 `task.max_concurrent`。
- **manager**：默认策略（`score`、`score_nopred`）选择时，节点在途任务数（Manager 自己的在途账，分配那一刻就计入，不等上报）达到其 `max_concurrent` 的，本次不作为候选。只影响“这一次选哪台”：不改分数、不进剔除和状态机，选中概率与急救触发的计算不变。全部候选都满时 `Allocate` 返回 `RESOURCE_EXHAUSTED`，错误信息带满载数。对照策略（`rr`、`wrr`）不变，保持“常见负载均衡器”的对照含义。
- **experiments**：用同一套参数复跑 `rr`、`wrr`、`score` 三组各 10 轮（推送关闭，`score` 与 `score_nopred` 等价），与 M10 结果对比。

## Capabilities

### Modified Capabilities
- `metric-protocol`：样本增加 `max_concurrent`。
- `worker-reporting`：上报带上并发上限。
- `manager-allocation`：候选集合增加“在途未满”条件；`Allocate` 错误信息带满载数。

## Impact

- **兼容性**：新字段号 12，老 Worker 不填（为 0）即不限制，行为与现在相同；老 Manager 忽略该字段。Redis key、MySQL 表不变。
- **不在本次范围**：按剩余名额加权（只做满/未满的硬条件）；Worker 自己上报“当前运行数”（Manager 的在途账更及时，且只有经 Manager 分配的任务）；对照策略的满载感知。
