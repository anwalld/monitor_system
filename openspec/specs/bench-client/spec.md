# bench-client Specification

## Purpose
压测客户端按固定随机序列向系统提交任务，观察任务在各节点间的分布和完成时间，用于 M5 验收和后续对照实验。

## Requirements

### Requirement: submit 模式
`lm-bench submit` SHALL 支持参数：`-m` Manager 地址、`-r` 每秒任务数（默认 5）、`-d` 持续秒数（默认 60）、`-s` 随机种子（默认 1）、`-c` client_id（默认 `bench`）、`--cmd`（默认 `/bin/sleep`）、`--args`（按空格拆分，默认 `0.2`）、`--bytes` 每个任务的 `input_bytes`（默认 1024）、`--token`、`--timeout` 任务超时秒数（默认 30）、`--csv` 明细输出文件，以及：
- `--policy`：填入 `AllocateReq.policy`（默认空，即本系统）。
- `--workload <文件>`：任务类型表，每行 `名称|比例|命令|参数1|参数2…`，`#` 开头和空行忽略；给出时替代 `--cmd`、`--args`。每个任务的类型 SHALL 按比例由独立随机流（种子 `seed + 1000003`）抽取，类型名填入 `AllocateReq.kind`。
- `--host-map <worker_addr>=<值>`（可重复）：Submit 前把参数中的 `{host}` 替换为该 Worker 地址对应的值；没有对应项时替换为 Worker 地址中的 IP。

每个任务 SHALL 依次调用 `Allocate`、`MarkSent`、直连 `worker_addr` 的 `Submit`（带 token 时附加 `authorization: Bearer <token>`）。任务的到达间隔 SHALL 服从速率为 r 的指数分布，由种子决定；同一种子两次运行的到达时刻序列 SHALL 相同，且与是否给出 `--workload` 无关。

#### Scenario: 固定种子
- **WHEN** 以 `-s 7 -r 10 -d 5` 运行两次
- **THEN** 两次各发起的任务数相同，到达时刻序列相同

#### Scenario: 任务类型
- **WHEN** 以同一种子、同一 `--workload` 运行两次
- **THEN** 两次每个任务的类型序列相同，各类型数量与比例相符

### Requirement: 输出
运行期间 SHALL 每秒打印一行：这一秒内各 Worker 地址的分配数与占比、累计完成数、累计失败数。结束时 SHALL 打印：完成时间（从 Allocate 到 Submit 返回）的 P50、P99、最大值，按阶段和错误码分类的失败数，各 Worker 的总占比；给出 `--workload` 时另按类型打印 P50、P99。给出 `--csv` 时 SHALL 每个任务一行：发起时刻（相对开始的毫秒）、task_id、worker_addr、阶段、状态码、exit_code、完成时间毫秒、任务类型（未给 `--workload` 时为空）。

#### Scenario: 汇总
- **WHEN** 两台 Worker 正常，运行结束
- **THEN** 输出包含每台 Worker 的占比，占比之和为 1；失败数为 0
