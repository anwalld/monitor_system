# Spec Delta

## MODIFIED Requirements

### Requirement: 来源选择与回退
Worker SHALL 支持配置项 `cpu_source`，取值为 `auto`、`shm`、`proc`、`cgroup`，默认 `auto`：
- `auto`：`/dev/cpustat` 能打开并映射时用共享页，否则用 `/proc/stat`，并记一条警告说明原因。
- `shm`：打不开或映射失败时，Worker MUST 以非 0 退出码退出并记录错误。
- `proc`：始终用 `/proc/stat`。
- `cgroup`：用 cgroup v2（`/sys/fs/cgroup`）的 `cpu.stat` 中 `usage_usec` 计算忙碌度，供按配额限核的容器节点使用。可用核数 q 取 `cpu.max` 的配额 / 周期；配额为 `max` 时取 `cpuset.cpus.effective` 的核数。Worker SHALL 上报 ceil(q) 个核，每核忙碌度都等于“两次读数间 usage 增量 / (经过时间 × q)”，截断到 [0, 1]。读不到 `usage_usec` 时 Worker MUST 以非 0 退出码退出并记录错误。

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

#### Scenario: 限核容器
- **WHEN** 容器以 `--cpus 2` 运行，Worker 以 `cpu_source: cgroup` 启动，容器内运行 `stress -c 2`
- **THEN** 启动日志写明可用 2 核，样本有 2 个核，`cpu_busy` 接近 1；同一 Docker 虚拟机的其他容器上的负载不计入
