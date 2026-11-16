# Design

## Context

- M1 的 `NodeTable` 已经由一把互斥锁保护，`CollectorService::Report` 在锁内更新最新样本，锁外把待写数据交给存储队列。
- 样本里有 `cpu_busy`、`runq_p99_us`（optional）、`mem_avail_ratio`、`nics[]`（rtt_ms、timeout_rate、retrans_rate），足够算出开发文档里四项评分。
- `Scheduler` 服务的代码在 M1 已经生成，没有实现。
- 开发文档“Manager 实现”给出了评分、剔除、选择的规则，“默认参数”表给出了起步值。本设计只补充文档没写死的细节。

## Goals / Non-Goals

**Goals**
- 每条上报在调度锁内算出分数和剔除状态，`Allocate` 按权重随机选一台。
- 规则全部是纯函数或小状态机，能用表驱动单测覆盖；抽样 10 万次频率误差 < 1%。
- 分数明细写进 `node:{id}`，为看板和 AI 问答留好数据。

**Non-Goals**
- 在途账和超时因子（m5b）；状态机、慢启动、急救、低频探测（M6）；预测因子（M9）。

## Decisions

### 1. 目录与模块

```
manager/scoring/
  scoring_params.{h,cpp}   ScoringParams：各指标参数，从 manager.yaml 的 scoring 段解析并校验
  score.{h,cpp}            normalize / normalize_log / nic_score / compute_score → ScoreBreakdown
  eviction.{h,cpp}         MetricGate（单指标迟滞去抖）、EvictionTracker（四个指标 + 原因）
  selector.{h,cpp}         Factors、weight()、pick_weighted(prefix, u)
manager/api/
  scheduler_service.{h,cpp}  Scheduler gRPC 服务：Allocate、MarkSent
```

- `lm_manager_scoring`：静态库，只依赖 `lm_proto` 和 `lm_common`，没有 gRPC 服务端代码，TSan 构建也能测。
- `lm_manager_ingest` 链接 `lm_manager_scoring`：`NodeState` 里存 `ScoreBreakdown` 和 `EvictionTracker`。
- `lm_manager_api`：`SchedulerService`，链接 `lm_manager_ingest`。

### 2. 线程与数据流

```
gRPC 线程（Report） ──锁内──▶ NodeTable::apply：更新样本 → compute_score → tracker.update
                    ──锁外──▶ 存储队列（AcceptedSample 带 ScoreBreakdown、evicted、reason）──▶ 存储线程写 node:{id}
gRPC 线程（Allocate）──锁内──▶ NodeTable::select：遍历节点 → 候选与权重 → 前缀和 → 二分 → 拷出 node_id、addr
                    ──锁外──▶ 生成 task_id、打日志、返回
```

- 随机数在锁外生成：`SchedulerService` 先取一个 [0, 1) 的均匀数 u 传进 `select`，锁内只做乘法和二分。随机数引擎每个服务一个 `std::mt19937_64`，用自己的小互斥锁保护（gRPC 线程池并发调用）。测试可以注入种子。
- 失联判断不靠后台线程：`select` 用调用时刻的单调时钟和 `last_seen_mono_ms` 比较。本变更没有调度主循环，M6 加上。

### 3. 归一化

- 线性：`s = clamp((bad − x) / (bad − good), 0, 1)`。好点比坏点小（CPU、RTT、超时率、重传率）或大（可用内存）都适用同一个公式，不需要方向标志。
- 对数（调度延迟 P99）：`s = clamp((ln bad − ln x) / (ln bad − ln good), 0, 1)`；x ≤ 0 时按好点处理（P99 为 0 只会出现在空直方图上，视为最好）。参数校验要求好点、坏点都 > 0。
- 不是有限数（NaN、Inf）的输入按 0 分处理，并计入 `ScoreBreakdown::invalid`，避免一条坏样本把节点抬成满分。

### 4. 网络分与缺失项

- 每块网卡：`min(s_rtt, s_timeout, s_retrans)`；节点：所有网卡里的最大值，`best_nic` 记下是哪块。
- 缺失项：`has_runq_p99_us()` 为 false，或 `nics` 为空，该项按 `scoring.missing_score`（默认 0.3）计。开发文档只规定了调度延迟这一种情况；网卡探测在 `nic_probe: auto` 下也可能降级为空，按同一规则处理：测不到的节点不该比测得到的节点更受青睐，而且一条规则比两条好记。
- 提示日志：节点第一次出现某项缺失时打一条 `info`（“节点 w1 未上报调度延迟，按 0.3 计分，建议升级 Worker”），之后不再重复；该项恢复上报后再次缺失会再打一次。

### 5. 加权

`score = Σ wₖ·sₖ / Σ wₖ`，默认权重 CPU 0.3、调度延迟 0.3、内存 0.2、网络 0.2。参数校验要求权重非负、和大于 0。

### 6. 关键指标剔除

每个关键指标一个 `MetricGate`，记 `evicted`、`bad_streak`、`good_streak`：

| 指标 | 越过剔除线 | 回到恢复线以内 | 10 倍 |
|---|---|---|---|
| CPU 忙碌度 | ≥ 0.97 | ≤ 0.85 | ≥ 9.7（不可能发生） |
| 调度延迟 P99 | ≥ 20 ms | ≤ 5 ms | ≥ 200 ms |
| 可用内存比例 | ≤ 0.05 | ≥ 0.1 | ≤ 0.005 |
| 网络：所有网卡中最小的超时率 | ≥ 0.5 | ≤ 0.2 | ≥ 5（不可能发生） |

- 未剔除时：越线则 `bad_streak++`，否则清零；`bad_streak ≥ 3` 或达到 10 倍立即剔除。
- 已剔除时：在恢复线以内则 `good_streak++`，否则清零；`good_streak ≥ 5` 恢复。处于恢复线和剔除线之间不算恢复（迟滞）。
- “10 倍”按该指标变坏的方向理解：越大越坏的指标是 ≥ 10 × 剔除线，越小越坏的是 ≤ 剔除线 / 10。CPU 和超时率是 0~1 的比例，10 倍不可能出现，只能走连续 3 次。
- 指标缺失（没有调度延迟、没有网卡）时，该指标的计数保持不变：不能因为测不到就判它恢复。
- 节点剔除 = 任一指标处于剔除状态；`evict_reason` 取第一个处于剔除状态的指标名（`cpu`、`runq`、`mem`、`net`），多个时用逗号连接。
- 失联：距上次上报 > 3000 ms（`scoring.lost_after_ms`）。不改 tracker，只在 `select` 时排除，也不写 Redis（M6 状态机会把它变成真正的状态并写进 `node:{id}`）。
- Worker 重启（M1 里 seq 变小且沉默超过 3 秒）不清空剔除状态：重启后的样本照样要连续 5 次正常才恢复。

### 7. 选择

- 候选：未剔除、未失联、`score > 0`。
- 权重：`w = score × f_inflight × f_timeout × f_slow × f_pred`。`Factors` 结构体放四个因子，本变更都取 1；m5b、M6、M9 只需填它，不改抽样代码。
- 抽样：节点按 `node_id` 排序后算前缀和（排序让同一种子下的结果可复现，20 台节点排序代价可忽略）。`target = u × total`，用 `std::upper_bound` 找第一个前缀和 > target 的位置。`total ≤ 0` 返回空。
- 返回值带上选中节点的权重和选中概率 `w / total`，用于日志，M6 判急救也要用。

### 8. Allocate

- 请求：`client_id` 不能为空、不超过 64 字节，否则 `INVALID_ARGUMENT`。`input_bytes`、`kind` 本变更只记日志，m5b 的在途字节数会用到 `input_bytes`。
- 返回：`task_id`、`worker_addr`（与 `node:{id}` 的 `addr` 一致）。
- 没有候选或权重和为 0：`RESOURCE_EXHAUSTED`，消息里带节点总数、剔除数、失联数，方便排查。
- `task_id` 格式：`<Manager 启动时的 Unix 毫秒，十六进制>-<自增序号，十进制>`，例如 `19a7c3e2f10-42`。Manager 重启后前缀变化，不会和重启前的 ID 撞上；不需要 UUID 库。
- `MarkSent` 返回 `UNIMPLEMENTED`，m5b 实现。
- 日志：每次分配打一条 `debug`（task_id、节点、概率），每 10 秒的汇总日志加上分配次数和拒绝次数。

### 9. 写入 Redis

`AcceptedSample` 增加 `ScoreBreakdown score`、`bool evicted`、`std::string evict_reason`，存储线程在原来的 `HSET` 里多写：

| 字段 | 例子 | 说明 |
|---|---|---|
| `score` | `0.8123` | 总分 |
| `s_cpu` `s_runq` `s_mem` `s_net` | `0.7143` | 各项分数，缺失项写实际计入的 0.3 |
| `best_nic` | `ens192` | 没有网卡时写空串 |
| `evicted` | `0` / `1` | 关键指标剔除 |
| `evict_reason` | `cpu,runq` | 未剔除时为空串 |

### 10. 配置

```yaml
scoring:
  missing_score: 0.3
  lost_after_ms: 3000
  evict_after: 3           # 连续越线次数
  recover_after: 5         # 连续恢复次数
  immediate_factor: 10
  cpu:  {good: 0.2,  bad: 0.9,  evict: 0.97,  recover: 0.85,  weight: 0.3}
  runq: {good: 100,  bad: 2000, evict: 20000, recover: 5000,  weight: 0.3}   # 微秒，取对数
  mem:  {good: 0.5,  bad: 0.1,  evict: 0.05,  recover: 0.1,   weight: 0.2}
  net:
    weight: 0.2
    rtt_ms:  {good: 5, bad: 100}
    timeout: {good: 0, bad: 0.1}
    retrans: {good: 0, bad: 0.05}
    evict: 0.5             # 所有网卡超时率都 ≥ 0.5
    recover: 0.2
```

校验：好点 ≠ 坏点；剔除线在坏点之外、恢复线在剔除线的好的一侧；对数项好点、坏点 > 0；权重非负且和 > 0；次数 ≥ 1。不合格时 Manager 启动失败，错误信息带键名。

## 性能与资源影响

- 采集开销、上报频率：不变。
- 调度锁：每条上报多一次 `compute_score`（4 块网卡以内约 30 次浮点运算和 2 次 `log`）和 4 次 gate 更新，< 1 µs；`Allocate` 锁内遍历 20 台、排序、前缀和、二分，< 5 µs。
- Redis：每条上报多 8 个字段，同一个 pipeline。MySQL 不涉及。

## Risks / Trade-offs

- **单台节点时总会被选中**：只要没剔除、没失联，权重再低也是唯一候选。这是开发文档的规则（全 0 才拒绝），压测时需要至少 2 台才能看到“转移”。
- **剔除状态在 Manager 重启后丢失**：调度状态只在内存里，重启后从样本重新累积，最多需要 3 秒才能再次剔除，可以接受。
- **CPU 和超时率的 10 倍规则不可能触发**：按文档字面实现，在设计里写明，对照实验后再决定是否改成“坏点之外再加一档”。

## Migration Plan

- 先部署新 Manager：Worker 不需要改。旧版 Worker（没有调度延迟或网卡数据）按缺失项 0.3 计分，仍可被选中。
- 回滚：换回 M3 版本的 Manager 镜像；`node:{id}` 里多出的字段无人读，不影响。

## 实现中补充的决定（2026-11-13）

### 11. 提前加入 `experiments/lm-bench`

虚拟机上没有 grpcurl，Manager 也没开 gRPC 反射。为了验收 `Allocate`，本变更先建了 `experiments/lm-bench`，只有 `allocate` 子命令（`-n` 次数、`-c` client_id、`-m` Manager 地址）。m5b 在它上面加 `submit` 压测模式。顶层 CMake 加了 `add_subdirectory(experiments)`，Manager 镜像的构建阶段相应多拷一个 `experiments` 目录。

### 12. TSan 覆盖

原来的 `build-tsan` 配置是 `LM_BUILD_MANAGER=OFF`，Manager 的测试从没进过 TSan。本变更另开 `build-tsan-m`（`-DLM_ENABLE_TSAN=ON -DLM_BUILD_MANAGER=ON`）跑 `test_manager`（不含 gRPC），以后 Manager 的并发代码都用这个目录检查。

### 13. 失联判定与在线计数用同一个时限

`main.cpp` 把 `NodeTable::Options::online_window_ms` 设成 `scoring.lost_after_ms`，汇总日志里的“在线”和 `select` 的“未失联”口径一致。
