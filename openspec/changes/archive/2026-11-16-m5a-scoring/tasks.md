# Tasks

## 1. 评分与剔除（manager/scoring，纯逻辑，各平台都编）

- [x] 1.1 实现 `scoring_params`：默认值取开发文档“默认参数”表，从 `scoring` 段解析并校验（好点 ≠ 坏点、剔除线在坏点之外、恢复线在剔除线好的一侧、对数项 > 0、权重非负且和 > 0、次数 ≥ 1）。验证：gtest 覆盖缺省值、覆盖部分字段、每类非法值报错且错误信息含键名
  > 2026-11-13 完成：`ScoringParams.*` 覆盖缺省值、部分覆盖、`config/manager.yaml` 可解析、11 类非法值（错误信息含键名）。
- [x] 1.2 实现 `score`：`normalize`、`normalize_log`、`nic_score`、`compute_score`（缺失项、非有限输入、最好的网卡）。验证：gtest 表驱动覆盖 spec 中归一化、网络分、加权与缺失项的全部场景
  > 完成：`Normalize.*`、`Score.*` 覆盖线性与截断、越大越好、对数映射（447.2 µs → 0.5）、非有限输入、网卡三项取最小、取最好网卡、全满分、缺失调度延迟 → 0.79、缺失网卡、P99 为 0 不算缺失。
- [x] 1.3 实现 `eviction`：`MetricGate`（迟滞、去抖、10 倍立即剔除、缺失保持）和 `EvictionTracker`（四个指标、原因串）。验证：gtest 覆盖 spec 中剔除与恢复的全部场景，以及越小越坏的内存指标、网络取最小超时率
  > 完成：`MetricGate.*`、`EvictionTracker.*` 覆盖连续 3 次、10 倍立即剔除、迟滞恢复（spec 原例）、内存越小越坏与 10 倍、非有限保持、缺失调度延迟不恢复、网络取最小超时率、多个原因 `cpu,net`。
- [x] 1.4 实现 `selector`：`Factors`、`weight()`、`pick_weighted()`。验证：gtest 覆盖边界场景（权重 1、0、1）、总权重为 0 返回空、固定种子抽样 10 万次频率误差 < 1%
  > 完成：`Selector.*` 覆盖边界（1、0、1）、末尾零权重不被选中、空或全零、因子相乘、10 万次抽样误差 < 1%。

## 2. 接入 Manager

- [x] 2.1 `NodeState` 增加分数和剔除状态，`NodeTable::apply` 在锁内重算，缺失项首次出现时返回提示（锁外打日志）；新增 `NodeTable::select(now_mono_ms, u)` 返回选中节点、权重、概率以及总数、剔除数、失联数。验证：gtest 覆盖剔除、失联节点不被选中，失联后重新上报回到候选，`AcceptedSample` 带出分数
  > 完成：`NodeTableScoring.*` 覆盖分数与缺失提示（只提示一次、恢复后再提示）、剔除后只选 w2、失联 3000/3001 ms 边界与重新上报、按 node_id 排序的加权边界、并发 apply 与 select。
- [x] 2.2 实现 `SchedulerService`（Allocate、MarkSent 返回 UNIMPLEMENTED，`task_id` 生成，随机数引擎加小锁，可注入种子），在 `main.cpp` 注册到同一个 gRPC 服务器，解析 `scoring` 配置；10 秒汇总日志加分配与拒绝次数。验证：进程内 gRPC 测试覆盖正常分配、序号递增、没有节点、全部剔除、client_id 为空与超长
  > 完成：`SchedulerService.*` 覆盖正常分配与 `19a7c3e2f10-1/-2`、没有节点、全部剔除（信息含“总数 1”“剔除 1”）、client_id 为空/65 字节/64 字节、MarkSent UNIMPLEMENTED、两台节点按权重分配。另加了 `experiments/lm-bench allocate` 用于虚拟机验收（见 design 第 11 节）。
- [x] 2.3 `RedisWriter` 写 `score`、`s_*`、`best_nic`、`evicted`、`evict_reason`。验证：`test_redis_writer`（需要本机 Redis，与现有用例同样在无 Redis 时跳过）检查字段；`config/manager.yaml` 补 `scoring` 段，Manager 能用它启动
  > 完成：`RedisWriterTest.ScoreAndEvictionFields`（临时 Redis 容器 lmtest-redis 上 10 项全过）；`config/manager.yaml` 已补 `scoring` 段，`ScoringParams.ManagerYamlParses` 覆盖。

## 3. 构建与验证

- [x] 3.1 macOS 与 Linux 容器构建并跑全部测试，再跑 TSan 构建（scoring 测试与 NodeTable 并发测试在 TSan 下运行）。验证：ctest 全部通过
  > 完成：macOS 159 项、Linux 容器 125 项全部通过；原 TSan 构建不编 Manager（`LM_BUILD_MANAGER=OFF`），另开 `build-tsan-m`（Manager 开启）跑 Manager 的非 gRPC 测试 36 项通过，含 `NodeTableScoring.ConcurrentApplyAndSelect`。
- [x] 3.2 【Linux】把新 Manager 镜像部署到 lm-worker-1 的 compose 项目，Worker 不动：`node:lm-worker-1` 出现分数字段且数值与当前样本手算一致；用 grpcurl 或测试客户端调 Allocate 得到 `172.16.255.133:50052`（或 Worker 上报所用地址）；在 Worker 上 `stress -c 4` 30 秒，`s_cpu` 下降到 0 附近，如 CPU 连续 3 次 ≥ 0.97 则 `evicted` 变 1、Allocate 返回 RESOURCE_EXHAUSTED，停掉 stress 5 秒后恢复。验证：记录字段与返回值
  > 完成：新镜像部署到 lm-worker-1（旧镜像留为 `lm-manager:m3`）。`node:lm-worker-1` 出现 score 1.0000、s_* 均 1.0000、best_nic ens192（ens160 的 8.8.8.8 经 NAT 有固定丢包，分数低一些）、evicted 0。Mac 上 `lm-bench allocate` 得到 `1a1d464f7fe-1 127.0.0.1:50052`（Worker 的 manager_addr 是 127.0.0.1，所以对端地址是回环，m5b 多节点时要改）；200 次全部选中它；client_id 为空返回 INVALID_ARGUMENT。`stress -c 4 -t 25`：第 3 条满载样本后 evicted 1、reason cpu、score 约 0.44（s_cpu 0、调度延迟分也下降），Allocate 返回“没有可用节点：总数 1，剔除 1，失联 0”；stress 结束后第 5 条正常样本恢复，日志各有一条剔除、恢复记录。
- [x] 3.3 更新 `README.md`（里程碑表、Manager 评分说明）和 `deploy/README.md`（`scoring` 配置、Allocate 调试命令）。验证：文档里的 `scoring` 配置与 `config/manager.yaml` 一致，能被解析单测覆盖
  > 完成：README（简介、目录、调试命令、查看数据、里程碑表）与 deploy/README 第 5 节（Manager 机）已更新；文档里的 `scoring` 段直接指向 `config/manager.yaml`，由 `ScoringParams.ManagerYamlParses` 覆盖。

## Workflow follow-up

- 提交前先问用户。
