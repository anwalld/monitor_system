# Spec Delta

## Purpose

压测客户端按固定随机序列向系统提交任务，观察任务在各节点间的分布和完成时间，用于 M5 验收和后续对照实验。

## ADDED Requirements

### Requirement: submit 模式
`lm-bench submit` SHALL 支持参数：`-m` Manager 地址、`-r` 每秒任务数（默认 5）、`-d` 持续秒数（默认 60）、`-s` 随机种子（默认 1）、`-c` client_id（默认 `bench`）、`--cmd`（默认 `/bin/sleep`）、`--args`（按空格拆分，默认 `0.2`）、`--bytes` 每个任务的 `input_bytes`（默认 1024）、`--token`、`--timeout` 任务超时秒数（默认 30）、`--csv` 明细输出文件。

每个任务 SHALL 依次调用 `Allocate`、`MarkSent`、直连 `worker_addr` 的 `Submit`（带 token 时附加 `authorization: Bearer <token>`）。任务的到达间隔 SHALL 服从速率为 r 的指数分布，由种子决定；同一种子两次运行的到达时刻序列 SHALL 相同。

#### Scenario: 固定种子
- **WHEN** 以 `-s 7 -r 10 -d 5` 运行两次
- **THEN** 两次各发起的任务数相同，到达时刻序列相同

### Requirement: 输出
运行期间 SHALL 每秒打印一行：这一秒内各 Worker 地址的分配数与占比、累计完成数、累计失败数。结束时 SHALL 打印：完成时间（从 Allocate 到 Submit 返回）的 P50、P99、最大值，按阶段和错误码分类的失败数，各 Worker 的总占比。给出 `--csv` 时 SHALL 每个任务一行：发起时刻（相对开始的毫秒）、task_id、worker_addr、阶段、状态码、exit_code、完成时间毫秒。

#### Scenario: 汇总
- **WHEN** 两台 Worker 正常，运行结束
- **THEN** 输出包含每台 Worker 的占比，占比之和为 1；失败数为 0
