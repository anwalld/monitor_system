# Tasks

## 1. Manager 在途账（manager/inflight，纯逻辑）

- [x] 1.1 实现 `InflightLedger`：任务表、确认/执行两条链表 + 索引、四种状态转移、确认超时（作废 / 未确认）、执行超时、迟到事件、每节点计数、基线与 T 的每秒更新、`factors(node)`；参数从 `inflight` 段解析并校验。验证：gtest 用模拟时钟覆盖 spec 中在途账的全部场景（分配、MarkSent 重新计时、幂等、NOT_FOUND、正常完成、没有 RECEIVED 直接完成、迟到完成、两种确认超时、执行超时、f_inflight 与 f_timeout 的数值、衰减）
  > 2026-11-13 完成：`InflightParams.*`、`Inflight.*` 共 13 项，覆盖 spec 全部场景（4096 字节记账、MarkSent 重新计时到 50 s 且幂等、移到队尾后按序超时、NOT_FOUND、正常完成、没有 RECEIVED 直接 FAILED、31 s 未确认、30 s 作废、310 s 执行超时后迟到 DONE、6 个在途 → 1/3、3 MiB → 1/3、基线 600 秒后收敛、T 半衰）。
- [x] 1.2 `NodeTable` 持有 ledger：`allocate()`（选择 + 记账一次加锁）、`mark_sent()`、`on_event()`、`tick()`、`tick_second()`；`select` 使用 ledger 的因子；`AcceptedSample` 带出在途快照。验证：gtest 覆盖“超时多的节点少分”场景（抽样频率）、在途堆积降权、并发 allocate/on_event/tick（TSan）
  > 完成：`NodeTableInflight.*`：两次执行超时后 w1 选中概率 exp(−1)/(exp(−1)+1)，10 万次抽样误差 < 1%；6 个在途权重 1/3 且快照随上报带出；3 线程 allocate/mark/event + tick 线程并发，结束后在途归零（TSan 通过）。

## 2. Manager 接口与主循环

- [x] 2.1 `SchedulerService` 改用 `NodeTable::allocate()`，实现 `MarkSent`；`CollectorService::OnTaskEvent` 销账；新增 `SchedulerLoop`（100 ms / 1 s）并在 `main.cpp` 启停；汇总日志加在途、超时、迟到计数。验证：进程内 gRPC 测试覆盖 MarkSent（OK、NOT_FOUND、INVALID_ARGUMENT）、OnTaskEvent 销账与迟到事件；SchedulerLoop 用短周期测试确认超时被处理
  > 完成：`SchedulerService.MarkSent`（OK、重复 OK、NOT_FOUND、INVALID_ARGUMENT）、`AllocateRecordsInputBytes`、`CollectorService.TaskEventsSettleLedgerAndLateIsOk`、`SchedulerLoop.ProcessesTimeoutsAndCounts`、`SchedulerLoop.ThreadRunsTicks`。
- [x] 2.2 `RedisWriter` 写 `inflight_tasks`、`inflight_bytes`、`unconfirmed`、`f_inflight`、`f_timeout`、`timeout_count`；`config/manager.yaml` 加 `inflight` 段。验证：Redis 集成测试检查字段
  > 完成：`RedisWriterTest.ScoreAndEvictionFields` 扩展检查 6 个在途字段；`config/manager.yaml` 加 `inflight` 段，`InflightParams.ManagerYamlParses` 覆盖。

## 3. Worker 任务服务（worker/task）

- [x] 3.1 实现 `TaskRunner`：posix_spawn、进程组、合并输出、尾部截断、超时杀进程组、取消、退出码约定。验证：gtest（macOS 与 Linux 都跑）覆盖正常退出码与输出、非 0、信号退出、超时杀掉孙进程、输出截断、命令不存在（−2）、取消
  > 完成：`TaskRunner.*`、`TailBuffer.*` 10 项（macOS、Linux、TSan 都过）：退出码与合并输出、最小环境变量、信号 → 143、1 秒超时杀掉后台 sleep、后台进程不拖住任务（主进程退出后 100 ms 收尾，见 spec 新增场景）、4096 字节截断、命令不存在 −2、取消 −3。
- [x] 3.2 实现 `EventSender`：有界队列、发送线程、重试 3 次、丢弃计数。验证：gtest 用进程内假 Collector 覆盖顺序送达、第一次 UNAVAILABLE 后重试成功、持续失败丢弃并计数、队列满丢最旧
  > 完成：`EventSender.*` 4 项：顺序送达、第一次 UNAVAILABLE 后第二次成功、连续失败 3 次后丢弃、容量 2 时丢最旧。用注入的发送函数测试，不经过 gRPC。
- [x] 3.3 实现 `TaskService`（Submit 校验、token、白名单、并发上限、事件、Probe、Rescue UNIMPLEMENTED）与 `task` 配置解析；`worker/main.cpp` 启动任务服务，Linux 上清空 ambient capability；更新 `config/worker.yaml` 和 `install-worker.sh` 的配置模板。验证：进程内 gRPC 测试覆盖 spec 中 Submit 的全部场景（假 Collector 检查 RECEIVED 先于 DONE/FAILED）；macOS 上非法白名单启动失败
  > 完成：`TaskConfig.*`、`TaskService.*`：RECEIVED 先于 FAILED/DONE、token 缺失/错误、白名单、task_id 空/129 字节、timeout 超限、被拒请求不上报事件、并发上限 1 时第二个 RESOURCE_EXHAUSTED 且只有 FAILED 事件、超时 −1、Probe。macOS 上 `allowed_commands: [sleep]` 报错含 `sleep`（`TaskConfig.DefaultsAndValidation`）。`config/worker.yaml` 和 `install-worker.sh` 模板加了注释掉的 `task` 段。

## 4. 压测客户端

- [x] 4.1 `lm-bench submit`：泊松到达、Allocate → MarkSent → Submit、stub 缓存、每秒占比、结束统计、`--csv`。验证：本机起 Manager + 进程内或本机 Worker（Linux 容器）跑 10 秒无失败；同一种子两次的任务数相同
  > 完成：Linux 容器里起 Manager + Worker，`submit -r 20 -d 5 -s 7` 112 个任务全部完成（P50 103 ms），同一种子两次都计划 112 个；token 错误时 16 个任务 Submit 返回 UNAUTHENTICATED。另加 `--print`（design 第 12 节）。

## 5. 构建与验收

- [x] 5.1 macOS、Linux 容器构建并跑全部测试；`build-tsan-m` 跑 Manager 与 Worker 的非 gRPC 测试。验证：ctest 全部通过
  > 完成：macOS 全部通过；Linux 容器 165 项通过；`build-tsan-m` 跑 Manager 与 TaskRunner 的非 gRPC 测试 63 项通过，无 TSan 告警。
- [x] 5.2 【Linux】部署：新 Manager 镜像到 lm-worker-1；用 `install-worker.sh` 部署新 Worker，`manager_addr` 改为 `172.16.255.133:50051`，开启 `task`（白名单 `/bin/sleep`、`/usr/bin/stress`、`/bin/grep`），配置 token。验证：`node:lm-worker-1` 的 `addr` 为 `172.16.255.133:50052`；从 Mac 提交 `/bin/grep Cap /proc/self/status` 得到 CapEff、CapAmb 为 0；不在白名单的命令被拒绝
  > 完成：新 Manager 镜像（旧的留为 `lm-manager:m5a`）和新 Worker 已部署，旧配置备份为 `/etc/lm/worker.yaml.m4.bak`。`manager_addr` 改为 172.16.255.133:50051，`node:lm-worker-1` 的 addr 变为 `172.16.255.133:50052`。开启 task（白名单 /bin/sleep、/usr/bin/stress、/bin/grep、/bin/sh，随机 token，配置文件 640 root:lm）。从 Mac 提交 `/bin/grep Cap /proc/self/status`：CapPrm、CapEff、CapAmb 都是 0，Worker 进程自己仍是 CapEff 000000c000002000。`/bin/rm` 返回 PERMISSION_DENIED，不带 token 返回 UNAUTHENTICATED。
- [x] 5.3 【Mac Docker】起第二个 Worker 容器 `lmtest-worker2`（lm-builder 镜像 + `build-linux` 产物，`cpu_source: proc`、`runq: off`、`nic_probe: off`，上报到 172.16.255.133:50051，发布 50052）。验证：Manager 日志出现新节点，`node:lmtest-worker2` 分数约 0.65；lm-bench allocate 1000 次两台占比与分数比例相符（误差 3% 以内）
  > 完成：`lmtest-worker2` 上报后 Manager 记为新节点，地址 `172.16.255.1:50052`（Mac 在 VMware NAT 网段的地址），核数 10，score 0.6500（s_cpu、s_mem 1.0，调度延迟和网络按 0.3）。allocate 2000 次：lm-worker-1 0.5975、worker2 0.4025；按分数算 0.9237/(0.9237+0.65) = 0.587，误差 1%。
- [x] 5.4 【Linux + Mac】转移验收：lm-bench 以 10 个/秒提交 `/bin/sleep 0.2` 共 120 秒，第 30–75 秒在 lm-worker-1 上 `stress -c 4`；再做一组 `stress -c 2`。验证：记录每 5 秒的占比曲线；`-c 4` 期间 lm-worker-1 占比降到 10% 以下、结束后 10 秒内回升；`-c 2` 期间占比部分下降；两组都没有失败，Manager 在途数最后回到 0、没有确认或执行超时
  > 完成：10 个/秒 `sleep 0.2`，120 秒，第 30–75 秒 stress。每 5 秒 lm-worker-1 占比：
  > - `stress -c 4`：前 30 秒 0.58–0.75（均值约 0.64）；30–35 秒 0.32；35–80 秒全部 0.00（CPU 剔除）；80–85 秒回到 0.46，之后 0.53–0.69。1232 个任务全部完成，P50 205 ms、P99 213 ms。
  > - `stress -c 2`（CPU 约 0.5，不剔除）：前 30 秒均值约 0.61，负载期间均值约 0.52，结束后约 0.61。按分数算（s_cpu ≈ 0.57，调度延迟分也略降）应降到约 0.55，方向和幅度一致。1232 个任务全部完成。
  > - 两组结束后 Manager 在途数回到 0，执行超时 0；“未确认 8”来自 5.2 故意提交的 8 个被拒任务（design 第 14 节）。
- [x] 5.5 【Linux】故障路径抽查：Worker 执行 `sleep 100` 时 `systemctl restart lm-worker`（应立即上报 FAILED，不产生执行超时）；再用 `systemctl kill -s KILL lm-worker`（事件丢失）→ 300 秒后执行超时、T 加一（见 design 第 13 节）；客户端只 Allocate 不提交 → 30 秒后作废、未确认不变。验证：记录 Manager 日志与 `node:{id}` 字段
  > 完成：
  > - `systemctl restart lm-worker`（lm-worker-1 上 8 个 `sleep 100`）：systemd 先 SIGTERM 整个 cgroup，8 个任务以 143 退出，FAILED 事件在 Worker 退出前发出，Manager 立即销账、执行超时 0；第 9 个因并发上限 8 返回 RESOURCE_EXHAUSTED 并上报 FAILED。
  > - `systemctl kill -s KILL lm-worker`（3 个 `sleep 100`）：客户端得到 UNAVAILABLE，事件全部丢失；Worker 2 秒后被 systemd 拉起。RECEIVED 后 300 秒（18:43:39.9–18:43:40.0）Manager 记 3 次执行超时，`node:lm-worker-1` 中 inflight_tasks 0、timeout_count 2.9965、f_timeout 0.2235（= exp(−0.5 × 3)）。
  > - 只 Allocate 不提交：5.3 的 2000 次 allocate 在 30 秒后全部作废（汇总日志“作废 2000”），未确认没有增加。
- [x] 5.6 清理 `lmtest-worker2` 容器；更新 `README.md`（里程碑、任务流程、lm-bench）和 `deploy/README.md`（`task` 配置、安全说明、验收命令、回滚）。验证：文档中的 `task` 配置能被解析单测覆盖
  > 完成：`lmtest-worker2` 已删除。README（简介、目录、调试命令、任务流程、里程碑）与 deploy/README（第 5 节在途参数、第 8 节任务服务：配置、安全、验收、单 VM 时的测试方法、回滚）已更新；`task` 配置结构与 `TaskConfig.*` 测试用的一致。

## Workflow follow-up

- 提交前先问用户。
- 3 台真实 Worker 到位后重做 5.4。
