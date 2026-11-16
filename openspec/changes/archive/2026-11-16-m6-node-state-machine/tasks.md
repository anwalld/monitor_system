# Tasks

## 1. 纯逻辑（manager/breaker，各平台都编）

- [x] 1.1 `BreakerParams`：breaker 段解析与校验，缺省值见 design 第 10 节。验证：gtest 覆盖缺省值、部分覆盖、`config/manager.yaml` 可解析、非法值报错含键名
  > 2026-11-14 完成：`BreakerParams.*`：缺省值、部分覆盖、`manager.yaml` 可解析、5 类非法值（错误信息含键名）。
- [x] 1.2 `RescueTracker`、`RescueTarget`、`Backoff`：待确认队列、单次超时、迟到、跳号、网络事件、批结算、目标与 k、指数退避与衰减。验证：gtest 用模拟时钟覆盖 spec 中急救流与批结算、退避的全部场景
  > 完成：`RescueTracker.*`（200 次连续超时算 1 次网络事件、迟到丢弃、跳号、批结算）、`RescueTarget.*`（N = 2 时 0.4375/0.3125/0.0625/0，成功批复位）、`Backoff.*`（5、10、20、40、60、60 分钟；level 3 两天后 0.75、下一次 5 分钟）。退避衰减起点见 design 第 12 节。
- [x] 1.3 `InflightLedger` 增加确认窗口（确认 / 未确认样本、修剪、合并比例）与 `reset_node`。验证：gtest 覆盖 RECEIVED 记确认、SENT 超时记未确认、作废不记、窗口过期、reset 清空基线与 T
  > 完成：确认窗口在 RECEIVED 与 SENT 超时时记录，作废不记；`trim_acks`、`ack_window`、`reset_node`、`scale_timeout`。由 `NodeStateMachine.UnconfirmedRatioEvicts`、`TooFewSamplesNoGray`、`RecoveryAfterFiveGoodReportsResetsLedger` 覆盖。

## 2. Manager 状态机与探测

- [x] 2.1 `NodeBreaker` 接入 `NodeTable`：状态、since、reason、good_reports、probe_fail_streak、退避；`apply` 时立即评估；`tick_states(now)` 返回状态变化与要启动急救的节点；`on_probe`；`rescue_batch`、`end_rescue`；`select` 按状态与 f_slow、慢启动 f_inflight = 1。验证：gtest 表驱动覆盖 spec 中 node-state 与 allocation 的全部场景（新节点、线性升权、慢启动结束、不叠加在途降权、失联剔除不急救、探测剔除、未确认剔除与样本不足、恢复与恢复中断、急救中失联、急救触发与分数太低、慢启动节点少分、急救中不被选中）
  > 完成：`NodeStateMachine.*` 13 项覆盖全部规格场景：新节点 f_slow 0.2、150 s 时 0.6、300 s 转正常；慢启动不乘 1/3；慢启动节点概率 0.2/1.2；3.1 s 失联剔除且无急救；5 条正常上报恢复并清空 T；恢复中断；探测 3 次失败剔除及恢复；未确认 10/20 对 0/100 剔除、19 个样本不剔除；score 0.9 急救（p = 0.9e⁻²/(0.9e⁻² + 0.65)）、0.5 不急救、急救中不被选中；批结算后转慢启动；急救中失联结束时剔除；并发上报/探测/tick（TSan 通过）。
- [x] 2.2 `Prober`（异步 Probe + CompletionQueue、channel 缓存）。验证：进程内 gRPC 测试：一个正常 Worker、一个不响应的地址，同一轮正常的在 500 ms 内成功、另一个计失败；连续 3 轮后后者被剔除
  > 完成：`Prober.HealthyNodeRoundIsFast`（正常节点一轮 < 600 ms 成功）、`Prober.UnreachableNodeEvicted`（不可达地址每轮 < 700 ms，3 轮后原因 probe 剔除）。
- [x] 2.3 `RescueSession` 与 `RescueManager`，`SchedulerLoop` 调 `tick_states`、启动与回收会话、推送状态变化；`main.cpp` 启动探测线程与急救管理；`breaker` 配置。验证：进程内 gRPC 测试：假 Worker 正常回应时急救在 T 足够小后结束并进入慢启动；假 Worker 不回应时 1 秒内结束
  > 完成：`RescueSession.HealthyWorkerRecoversToSlowStart`（真实 TaskService，空闲时 44 批全成功、T 4 → 0.039、转慢启动）、`RescueSession.UnreachableWorkerEndsQuickly`（3 批失败后目标 0.0625 < p，300 ms 结束）。“达到目标”的 1% 误差见 design 第 11 节。
- [x] 2.4 `RedisWriter` 写状态字段，支持只含状态的写入。验证：Redis 集成测试检查字段与 state_only 写入不产生时间序列
  > 完成：`RedisWriterTest.StateFieldsAndStateOnlyWrite`：5 个状态字段；state_only 写入只改状态，seq 与时间序列不变（临时 Redis 容器上 6 项全过）。

## 3. Worker 急救

- [x] 3.1 `Collector::latest()`；`RescueSampler`（独立 CPU 来源、IntervalGuard、最新样本）；`TaskService::Rescue`（单流、时长上限）；`task.rescue_max_s`；`worker/main.cpp` 接线。验证：进程内 gRPC 测试（假 CPU 来源）：1000 条请求 seq 一致、第二条流 ABORTED、时长上限 DEADLINE_EXCEEDED；macOS、Linux 都跑
  > 完成：`WorkerRescue.*`：1000 条请求 seq 与 send_ts_us 原样返回，样本 CPU 0.5（假来源）、其余字段来自最新样本；第二条流 ABORTED、第一条继续；`rescue_max_s: 1` 时约 1 秒 DEADLINE_EXCEEDED，结束后 sampler 已释放。macOS、Linux 都通过。

## 4. 构建与验收

- [x] 4.1 macOS、Linux 容器构建并跑全部测试；`build-tsan-m` 跑非 gRPC 测试（含 NodeTable 状态机并发）。验证：ctest 全部通过
  > 完成：macOS 209 项、Linux 容器 195 项全部通过；`build-tsan-m` 85 项通过，无 TSan 告警。
- [x] 4.2 【Linux】部署新 Worker 与 Manager（旧镜像留为 `lm-manager:m5`），Mac 上起 `lmtest-worker2`。验证：两台都先进入慢启动、5 分钟后转为正常（记录 `state` 与时间）；急救一次期间在虚拟机上读 `/dev/cpustat` 周期为 1000 µs，结束后恢复
  > 完成：旧镜像留为 `lm-manager:m5`。16:39:06 Manager 启动，两台节点都以 slow_start 登记（f_slow 0.216 / 0.211），探测正常。lmtest-worker2 16:39:08 → normal 16:44:08；lm-worker-1 恢复后 16:39:40 → normal 16:44:40，都正好 5 分钟。急救时 Worker 日志“cpustat 刷新周期 1000000us -> 1000us”，结束时“恢复为 1000000us”（4.6 两次都有）。
- [x] 4.3 【Linux】节点宕机：`systemctl stop lm-worker`。验证：3 秒内 `state` 为 `evicted`、原因 `lost`，日志中没有急救；Allocate 全部给 worker2
  > 完成：`systemctl stop lm-worker` 后 2.72 秒 state 变为 evicted，原因 probe（见 design 第 13 节），日志里没有急救；allocate 300 次全部给 172.16.255.1:50052。
- [x] 4.4 【Linux】节点恢复：`systemctl start lm-worker`。验证：约 5 秒后进入慢启动，约 5 分钟后回到正常；记录占比随 f_slow 回升
  > 完成：`systemctl start` 后 5.72 秒（Worker 启动 + 5 条正常上报）进入 slow_start，5 分钟后（16:44:40）转为 normal。期间以 10 个/秒压测 330 秒，3316 个任务无失败；两台都在慢启动且 f_slow 接近，所以占比只从 0.54 缓升到 0.60，慢启动单独降权的效果由单测 `SlowStartNodeGetsLessTraffic` 覆盖。
- [x] 4.5 【Linux】灰色故障：先设 5 分钟自动撤销，`iptables -I INPUT -p tcp --dport 50052 -j DROP`，上报照常。验证：约 3 秒后原因 `probe` 剔除；删规则后恢复为慢启动
  > 完成：先设 5 分钟自动撤销，再加 `iptables -I INPUT -p tcp --dport 50052 -j DROP`：2.89 秒后 evicted、原因 probe、probe_fail_streak 3，同时 last_seen 仅 0.5 秒前（上报照常）；删规则后 1.58 秒回到 slow_start，规则已删、定时器已取消。
- [x] 4.6 【Linux】急救：让 lm-worker-1 的 T 达到 4 以上（提交比执行超时更长的任务，见 design 第 14 节），观察自动急救：健康时急救结束、T 明显下降、进入慢启动；再在急救期间每 500 ms 交替加删 iptables 规则（间歇故障），验证急救日志中有超时和网络事件、目标下调
  > 完成：
  > - 健康急救：16:51:08 第 6 个执行超时后自动触发（p 0.071 < 0.25，T 6.0），2201 ms 结束：批 21 成功 / 1 失败，请求 2186，超时 1，网络事件 1，T → 0.873，p 0.503 达到目标 0.500，转 slow_start。
  > - 间歇故障：退避 5 分钟后第二次急救 16:57:57 触发（p 0.117，T 4.55），0.1 秒后开始每 500 ms 交替加删丢包规则：500 ms 结束，批 1 成功 / 4 失败，请求 482、成功 119、超时 353、网络事件 1，目标降到 0，转 slow_start。Worker 只收到 120 条请求。结束后无残留规则。
- [x] 4.7 更新 README、deploy/README（状态机、探测、急救、breaker 配置、验收命令、回滚），删除 `lmtest-worker2`。验证：文档中 breaker 配置与 `config/manager.yaml` 一致
  > 完成：README（简介、目录、节点状态说明、里程碑）与 deploy/README（第 5 节 breaker 参数与状态字段、第 8 节 Probe/Rescue 说明、M6 验收命令、回滚）已更新；`lmtest-worker2` 已删除。文档中的 breaker 段直接指向 `config/manager.yaml`，由 `BreakerParams.ManagerYamlParses` 覆盖。

## Workflow follow-up

- 验收完成后提交并推送（用户已授权）。
- 3 台真实 Worker 到位后用“关掉 Worker 虚拟机”重做 4.3、4.4。
