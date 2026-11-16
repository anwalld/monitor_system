# Proposal

## Why

对应开发文档里程碑 M3。CPU 忙碌度只说明核有多忙，说明不了任务要排队多久才能上 CPU。8 个线程挤在一个核上，和 1 个线程打满一个核，忙碌度都是 1.0，但前者的排队延迟高出几个数量级。开发文档把调度延迟 P99 作为评分的四项指标之一（权重 0.3），也作为剔除条件。M1 的协议已经预留了 `runq_p50_us`、`runq_p99_us`、`runq_drop_rate` 三个可选字段，但一直没有填。M3 用 eBPF 在调度跟踪点测量排队延迟并上报，为 M5 的评分和剔除提供数据。

## What Changes

- 新增 `bpf/`：
  - `runqlat.bpf.c`：libbpf + CO-RE，挂在 `tp_btf/sched_wakeup`、`sched_wakeup_new`、`sched_switch` 三个跟踪点上。三个 map 是：入队时间 HASH（10240 项）、对数线性直方图 PERCPU_ARRAY（208 桶）、丢弃计数 PERCPU_ARRAY。
  - `runqlat.h`：分桶函数和常量，内核侧与用户态共用。
  - 构建时用 bpftool 生成 `vmlinux.h` 和 skeleton，嵌进 Worker 二进制。
- Worker 新增调度延迟采集：每秒把各核的直方图和丢弃计数求和，与上一秒做差，在桶内线性插值得到 P50、P99，并算出丢弃率，填进样本的三个可选字段。
  - 新增配置项 `runq: auto | on | off`，默认 `auto`：加载失败时记警告、不填这三个字段，Worker 照常运行。
  - 首个周期只建立基线。
- 新增对照工具：用同一个 eBPF 程序统计一段时间内的分布，按 2 的幂重新分桶输出，与 bcc `runqlat` 对比。
- Manager 把三个调度延迟字段写进 Redis Hash `node:{id}`，字段未上报时不写。JSON 形式的样本（`node:{id}:ts`、`stream:metrics`）已经会自动带上这三个字段。
- 构建与部署：
  - 构建镜像增加 clang、libbpf-dev、bpftool。
  - CMake 在 Linux 上找到这些工具时才编译 eBPF 部分，macOS 上自动跳过。
  - Worker 运行时增加 libbpf1。systemd 单元已经授予 CAP_BPF 和 CAP_PERFMON，不需要改。

## Capabilities

### New Capabilities
- `worker-runq-latency`：Worker 测量调度排队延迟的口径、分桶和分位数算法、丢弃率、周期差分、启用与降级行为，以及读取开销。

### Modified Capabilities
- `manager-hot-storage`：`node:{id}` Hash 增加 `runq_p50_us`、`runq_p99_us`、`runq_drop_rate` 三个字段（有上报时才写）。
  - 这个能力由 M1 的 `m1-skeleton` 引入，M1 还没有归档，`openspec/specs` 里没有它，所以本变更用 ADDED 增量追加一条要求。
  - 归档顺序：先归档 M1，再归档 M3。

## Impact

- 对应里程碑：M3。影响模块：bpf（新增）、worker、manager（storage）、deploy。
- proto 不改：三个字段是 M1 已定义的 optional 字段，老版本 Manager 收到后照常解析。
- Redis：`node:{id}` 新增三个字段，只增不改，读方不受影响。MySQL 不涉及。
- 新的构建依赖：clang（≥ 14）、libbpf-dev（1.x）、bpftool。新的运行依赖：libbpf1。目标主机需要 BTF（M0 自检已覆盖）。
- 不在范围内：
  - Manager 根据调度延迟评分和剔除（M5）。
  - 按 cgroup 或进程过滤、按核分别统计。
  - 内核版本低于 6.8 时 `task_struct.__state` 字段名的兼容处理（目标环境固定为 6.8）。
