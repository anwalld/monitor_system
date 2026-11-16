# Spec Delta

## Purpose

Worker 接收客户端直接提交的任务，在本机执行白名单里的命令，把接收与完成事件上报给 Manager，并把结果返回给客户端。

## ADDED Requirements

### Requirement: 任务服务的启用与配置
Worker SHALL 只在 `task.enabled: true` 时在 `task.listen`（默认 `0.0.0.0:50052`）提供 `WorkerTask` 服务。配置项及默认值：`allowed_commands: []`、`token: ""`（环境变量 `LM_TASK_TOKEN` 非空时覆盖）、`max_concurrent: 0`（表示 2 × 核数）、`default_timeout_s: 300`、`max_timeout_s: 3600`、`output_tail_bytes: 4096`。`allowed_commands` 中的项 MUST 是绝对路径，否则 Worker 启动失败并在错误信息中给出该项。

#### Scenario: 默认关闭
- **WHEN** 配置中没有 `task` 段
- **THEN** Worker 不监听 50052 端口，其余行为与 M4 相同

#### Scenario: 非法白名单
- **WHEN** `allowed_commands` 包含 `sleep`
- **THEN** Worker 启动失败，错误信息包含 `sleep`

### Requirement: Submit 请求校验
`WorkerTask.Submit(TaskReq{task_id, command, args, timeout_s})` SHALL 依次校验，失败时不执行命令、不上报事件：
- task_id 为空或超过 128 字节：`INVALID_ARGUMENT`。
- 配置了 token 而 metadata `authorization` 不等于 `Bearer <token>`：`UNAUTHENTICATED`。
- `command` 不在 `allowed_commands` 中（完全匹配）：`PERMISSION_DENIED`。
- `timeout_s` 超过 `max_timeout_s`：`INVALID_ARGUMENT`。

#### Scenario: 不在白名单
- **WHEN** 白名单为 `[/bin/sleep]`，提交 `/bin/rm`
- **THEN** 返回 `PERMISSION_DENIED`，不执行任何命令

#### Scenario: token 错误
- **WHEN** 配置 token 为 `s3cret`，请求不带 authorization
- **THEN** 返回 `UNAUTHENTICATED`

### Requirement: 并发上限
同时执行的任务数达到 `max_concurrent` 时，新的 Submit SHALL 上报 FAILED 事件并返回 `RESOURCE_EXHAUSTED`，不执行命令。

#### Scenario: 超过并发
- **WHEN** `max_concurrent` 为 1，一个 `/bin/sleep 2` 正在执行时再提交一个任务
- **THEN** 第二个返回 `RESOURCE_EXHAUSTED`，Manager 收到它的 FAILED 事件

### Requirement: 执行与结果
通过校验的任务 SHALL：
1. 先把 RECEIVED 事件放入发送队列（不等待发送完成）。
2. 用 `posix_spawn` 执行 `command args...`：子进程自成进程组，stdin 为 `/dev/null`，stdout 与 stderr 接到同一管道，环境变量只有 `PATH=/usr/local/bin:/usr/bin:/bin` 和 `LM_TASK_ID`。
3. `timeout_s` 为 0 时使用 `default_timeout_s`；到时 SHALL 向整个进程组发送 SIGKILL。客户端取消请求时同样杀掉进程组，返回 `CANCELLED`。
4. 主进程退出后，若有后台子孙进程仍占着输出，再等 100 ms 收尾，然后杀掉整个进程组：任务以主进程为准。
5. 返回 `TaskResult`：`exit_code`（正常退出为退出码；被信号杀死为 128 + 信号；超时为 −1；无法启动为 −2；被取消为 −3，此时 gRPC 返回 `CANCELLED`）、`stdout_tail`（输出的最后 `output_tail_bytes` 字节）、`exec_ms`（从启动到回收子进程）。
6. `exit_code` 为 0 时上报 DONE，否则上报 FAILED；事件带 `exec_ms`。

#### Scenario: 正常执行
- **WHEN** 提交 `/bin/sh -c "echo hi; exit 3"`（`/bin/sh` 在白名单中）
- **THEN** 返回 OK，`exit_code` 为 3，`stdout_tail` 为 `hi\n`；Manager 先后收到 RECEIVED 和 FAILED

#### Scenario: 超时
- **WHEN** 提交 `/bin/sh -c "sleep 100 & sleep 100"`，`timeout_s` 为 1
- **THEN** 约 1 秒后返回 `exit_code` −1，两个 sleep 进程都已退出

#### Scenario: 后台进程不拖住任务
- **WHEN** 提交 `/bin/sh -c "sleep 100 & echo started"`
- **THEN** 1.5 秒内返回，`exit_code` 为 0，`stdout_tail` 为 `started\n`

#### Scenario: 输出截断
- **WHEN** 命令输出 10000 字节，`output_tail_bytes` 为 4096
- **THEN** `stdout_tail` 是最后 4096 字节

### Requirement: 子进程不继承 Worker 的能力
在 Linux 上，Worker SHALL 在启动任务服务前清空自身的 ambient capability 集合，使任务子进程的有效能力集为空。

#### Scenario: 能力隔离
- **WHEN** 以 systemd 单元（ambient 授予 CAP_NET_RAW、CAP_BPF、CAP_PERFMON）运行的 Worker 执行 `/bin/grep Cap /proc/self/status`
- **THEN** 输出中 `CapEff` 与 `CapAmb` 都为 0

### Requirement: 任务事件可靠发送
任务事件 SHALL 由独立线程按入队顺序发送 `Collector.OnTaskEvent`，每次截止 500 ms，失败时重试，每个事件最多尝试 3 次（重试前分别等 200 ms、400 ms），仍失败则丢弃并计数。队列容量 1024，满时丢弃最旧的事件并计数。

#### Scenario: Manager 短暂不可用
- **WHEN** Manager 第一次调用返回 UNAVAILABLE，第二次成功
- **THEN** 事件只送达一次，丢弃计数不变

### Requirement: Probe
`WorkerTask.Probe(ProbeReq{seq})` SHALL 在任务服务开启时立即返回 `ProbeResp{seq}`，不校验 token。`WorkerTask.Rescue` SHALL 返回 `UNIMPLEMENTED`。

#### Scenario: 探测
- **WHEN** 调用 Probe，seq 为 42
- **THEN** 返回 seq 42
