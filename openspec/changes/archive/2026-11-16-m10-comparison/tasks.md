# Tasks

## 1. Manager：对照策略

- [x] 1.1 `task.proto` 的 `AllocateReq` 加 `string policy = 4`；`NodeTable` 增加 `select_baseline_locked`（rr、wrr，候选：fresh 且 probe 未到剔除线）与 `allocate(..., Policy)`；`score_nopred` 置 `f_pred = 1`；`manager.yaml` 新增 `allocation.allow_policy`。验证：`test_node_table` 新增用例——rr 顺序与跳过失联、rr 不看关键指标剔除、wrr 4:2 的序列、score_nopred 忽略预测
- [x] 1.2 `SchedulerService::Allocate` 解析 `policy`（未知 → INVALID_ARGUMENT，关闭 → FAILED_PRECONDITION）。验证：`test_manager_rpc` 进程内 gRPC 用例；本机 `ctest` 全过
- [x] 1.3 【Linux】`lm-builder` 容器里构建并跑 `ctest`；构建 `lm-manager` 镜像
  - 结果：240 项全过（Redis/MySQL 集成测试未设环境变量，跳过；本变更不涉及存储）。镜像构建成功

## 1b. Worker：cgroup CPU 来源

- [x] 1.4 `CgroupCpuSource`（`cpu.max` / `cpuset.cpus.effective` / `cpu.stat` 解析，合成每核读数）、`cpu_source: cgroup`。验证：`test_proc_readers` 新增解析与目录用例；lmtest-worker2 以 `--cpus 2` 重建后日志写明可用 2 核，Manager 看到 `ncpu = 2`
  - 结果：4 个用例通过；重建命令（三节点实验时又加了 `--init`、端口只发布在 `172.16.255.1:50052`）`docker create --name lmtest-worker2 --cap-add NET_ADMIN --cpus 2 -p 50052:50052 -v $PWD/build-linux:/b:ro -v ~/Backups/monitor_system/lmtest-worker2.yaml:/w.yaml:ro lmtest-w2img /b/worker/worker -c /w.yaml` 后 `docker network connect lmtest-net2`；日志“cgroup v2（可用 2.00 核，上报 2 核）”，`/api/nodes` 里 `ncpu: 2`，空闲 cpu 0.002。重建后网卡顺序变成 eth0 = lmtest-net2、eth1 = bridge，配置的探测目标随之对调；任务并发上限设为 4

## 2. 压测客户端

- [x] 2.1 `lm-bench`：`--policy`、`--workload`、`--host-map`，`allocate` 子命令也支持 `--policy`；CSV 加 `kind` 列；汇总按类型分位数。验证：同种子两次运行类型序列相同；不带 `--workload` 时到达序列与改动前一致
  - 结果：到达序列仍由原随机流生成（类型用独立流 seed+1000003），试跑中同一轮四组的任务数都是 460
- [x] 2.2 `experiments/compare_workload.txt`、`compare_data_server.py`。验证：两个节点上各手工提交三类任务都成功，记下空闲时的耗时
  - 结果（空闲，lm-worker-1 / lmtest-worker2）：cpu（45 万次循环）0.27 / 0.29 s；io（800 × 4 KB dsync）0.41 / 0.44 s（1000 次时）；net 2 MB 0.23 / 0.87 s（容器经 host.docker.internal 转发较慢）。`dd oflag=direct` 大块写在两边都只要几十毫秒（宿主 SSD），所以 IO 型改成小块同步写

## 3. 实验脚本

- [x] 3.1 `experiments/compare.py`：轮次与组顺序、干扰计划（种子决定）、注入与撤销（ssh / docker exec，netem 带自动撤销）、等待节点恢复、调用 `lm-bench`、保存明细与注入记录、Ctrl-C 清理。验证：`--dry-run` 打印计划；两节点 1 轮 × 4 组 × 60 秒跑通
  - 试跑（速率 8/s，CPU 干扰在 lmtest-worker2 上 25 秒）：P50 rr 530、wrr 442、score_nopred 297、score 287 ms；干扰期 P99 rr 1630、wrr 1410、score_nopred 1055、score 1052 ms；反应时间 score_nopred 14.0 s、score 6.5 s，rr/wrr 未降。失败：rr 29（容器并发上限 4 被占满）、score 45（其中 36 个是 55–60 秒两节点同时被剔除，Allocate 返回 RESOURCE_EXHAUSTED）
- [x] 3.2 `experiments/compare_stats.py`：每次运行的指标、反应时间、按组汇总、Cohen's d 与 Welch t（正态近似）、按类型 P99，输出 Markdown 与 JSON。验证：用 3.1 的结果出表；构造一份已知占比的 CSV 检查反应时间计算
  - 结果：0.1 秒一个分配、占比 43 秒时从 0.5 降到 0.2，算出 7.5 秒（含 5 秒窗口滞后约 4.2 秒）；不降的序列记“未降”

## 4. 部署与实验

- [x] 4.1 【Linux】部署新 Manager（留回滚标签 `lm-manager:m9`）；构建 `lm-ai:m10push`（打开 `predict.push`）并换上。验证：`lm-bench allocate --policy rr -n 10` 在两节点间交替；Manager 日志有预测推送
  - 结果：回滚标签 `lm-manager:m9`、`lm-ai:m9`；`lm-ai:m10push` 在虚拟机上由 `lm-ai:latest` 派生（`USER root` 改 `/etc/lm/ai.yaml` 后切回 `lm`），打成 `lm-ai:latest` 后 compose 重建 ai。`allocate --policy rr -n 6` 3:3，`wrr` 4:2，`foo` 返回 INVALID_ARGUMENT。Manager 日志“预测批”开始增长
- [x] 4.2 【Linux】四组 × 10 轮正式实验（三节点：lm-worker-1 + lmtest-worker2/3）。验证：40 次运行都有明细与注入记录
  - 第一次（两节点，13:31–15:07）作废，原因见 design「实现中的决定」1。
  - 第二次 15:10–17:20，40/40 完成，每次 150 秒、速率 8/s、CPU 干扰 60 秒（lm-worker-1 上 3 轮、lmtest-worker2 上 5 轮、lmtest-worker3 上 2 轮）。明细在 `experiments/results/m10-3node/`，汇总 `summary.md`、`summary.json`。期间 lm-worker-1 上的 ROS 仿真一直在跑（load 8–12），本系统各组几乎不给它派任务
- [x] 4.3 换回 `lm-ai:latest`（推送关闭）；结果写进本文件与 README“对照实验”。验证：统计表、结论与局限
  - 结果（均值 ± 标准差，10 轮）：

    | 组 | P50 ms | P99 ms | 干扰期 P99 ms | 失败/次 | 反应时间 |
    |---|---|---|---|---|---|
    | rr | 437 ± 132 | 1862 ± 854 | 2336 ± 1196 | 36.7 | 0/10 次降到一半 |
    | wrr | 432 ± 92 | 2228 ± 1916 | 2392 ± 2101 | 18.9 | 0/10 |
    | score_nopred | 434 ± 101 | 1427 ± 444 | 1773 ± 1026 | 101.7 | 10/10，中位 5.8 s |
    | score | 427 ± 93 | 1706 ± 602 | 1768 ± 546 | 115.6 | 10/10，中位 6.0 s |

  - 结论：
    - **反应**：本系统 20 次干扰全部在 4–7.5 秒内把该节点占比压到一半以下（含 5 秒窗口滞后），干扰期占比接近 0；轮询、加权轮询始终按固定比例派。
    - **完成时间**：P50 四组一样（≈ 430 ms，任务本身耗时为主）。本系统的 P99 比轮询低 156–434 ms、干扰期 P99 低约 565 ms（−24%），按任务类型 CPU、IO 型的 P99 都更低；但轮间波动大（Cohen's d 0.2–0.6，Welch p 0.15–0.64），10 轮、三节点下**不显著**。
    - **失败更多**：本系统各组每次约 100–115 个失败，几乎全是 Worker 并发上限（容器 4、虚拟机 8）满了拒收（Submit RESOURCE_EXHAUSTED），另有约 5 个/次是三台同时剔除时 Allocate 被拒。原因：lm-worker-1 被 ROS 占着、分数低，再剔除一台被干扰的容器后，几乎全部任务压到剩下那个 2 核容器上；评分和 `f_inflight` 不知道 Worker 的并发上限，在途量相对自身基线升高时降权太慢。轮询类把任务分散到三台，反而少撞上限。
    - **预测**：`score` 与 `score_nopred` 无差别（干扰期 P99 差 5 ms，p 0.99），与 M9 “预测未优于基线”一致，`predict.push` 维持关闭。
  - 建议（不在本变更里改）：把 Worker 的并发上限纳入选择（上报 `max_concurrent` 与当前运行数，满了视为不可选或大幅降权），或让 Manager 根据 Worker 的拒收事件降权；加机器后用同一套工具重跑。
  - 收尾：虚拟机上 `lm-ai:latest` 已换回 `lm-ai:m9`（`push: false`），`lm-ai:m10push` 保留；Mac 上的下载服务已停
