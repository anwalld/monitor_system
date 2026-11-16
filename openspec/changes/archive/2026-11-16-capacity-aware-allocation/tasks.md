# Tasks

## 1. 协议与 Worker

- [x] 1.1 `monitor.proto` 的 `Sample` 追加 `uint32 max_concurrent = 12`；`Reporter::Options` 增加 `max_concurrent`，发送前填入；`main.cpp` 在任务服务开启时传入解析后的上限。验证：`test_reporter`（或现有上报测试）新增用例——sink 收到的样本带上限；`--dry-run` 输出含 `maxConcurrent`
  - 结果：`Reporter.AttachesMaxConcurrent` 通过（上限 4 → 样本 4，未设 → 0）。端到端改用 3.1 的分配测试从 Manager 侧确认（没有单独跑 `--dry-run`）；`Allocate` 错误信息由 `SchedulerService.FullNodesInErrorMessage` 覆盖

## 2. Manager

- [x] 2.1 `NodeState` 记录 `max_concurrent`；`select_locked` 跳过在途任务数 ≥ 上限的节点，`SelectResult` 增加 `full` 计数；`Allocate` 错误信息带满载数；对照策略不变。验证：`test_node_table` 新增用例——满了不选、DONE 后恢复、上限 0 不限制、全满时 `full` 计数、满载不改变 `n.prob`（状态机概率）、rr 不受影响
  - 结果：`NodeTableCapacity` 3 个用例通过
- [x] 2.2 【Linux】`lm-builder` 容器里构建并跑 `ctest`；构建 `lm-manager` 镜像
  - 结果：244 项全过（Redis/MySQL 集成测试未设环境变量，跳过）；镜像 36 秒构建完成

## 3. 部署与复跑

- [x] 3.1 【Linux】部署 Manager（回滚标签 `lm-manager:m10`）、虚拟机 Worker（`deploy/install-worker.sh`）、两个测试容器（重启即用新二进制）。验证：`/api/nodes` 正常，Manager 日志无异常；Redis `node:{id}` 里能看到上限（如有写入）或用 `worker --dry-run` 确认字段
  - 结果：虚拟机 Worker 直接换二进制（旧的留作 `/opt/lm/bin/worker.m10`，没用 install-worker.sh：它会装包、重编内核模块，这次用不着），日志“并发上限 8”；两个容器重启。`lm-bench allocate -n 30`（不发送）：lm-worker-1 8 个、两个容器各 4 个，其余 14 个返回“没有可用节点：总数 3，剔除 0，失联 0，在途已满 3”
- [x] 3.2 复跑 `rr`、`wrr`、`score` 各 10 轮（参数与 M10 相同，`caffeinate`，单次超过 300 秒报警），与 M10 结果对比失败数、P99、干扰期 P99、反应时间。验证：30 次运行完成，对比表写进本文件与 README
  - 运行：17:46–19:16，30/30 完成，明细 `experiments/results/cap-3node/`（汇总 `summary.md`、`summary.json`）。种子、干扰计划与 M10 相同（seed 1000–1009）。
  - 结果（均值 ± 标准差，10 轮；括号内为 M10 同组）：

    | 组 | P50 ms | P99 ms | 干扰期 P99 ms | 失败/次 | 反应 |
    |---|---|---|---|---|---|
    | rr | 337 ± 61（437） | 1372 ± 1087（1862） | 1520 ± 1070（2336） | 15.8（36.7） | 0/10 |
    | wrr | 316 ± 30（432） | 1179 ± 542（2228） | 1465 ± 925（2392） | 12.6（18.9） | 0/10 |
    | score | 321 ± 18（427） | 924 ± 124（1706） | 1010 ± 136（1768） | 10.8（115.6） | 10/10，中位 6.8 s |

  - 结论：
    - **失败**：本系统从每次 115.6 降到 10.8，三组里最少；`Submit` 被 Worker 拒收从 M10 的 1103 次（10 轮合计）降到 0，剩下的 105 次是三台同时满载时 `Allocate` 直接返回 RESOURCE_EXHAUSTED（快速失败，不占 Worker）。逐轮配对：7/10 轮失败少于轮询。
    - **完成时间**：P99 比轮询低 448 ms（−33%）、干扰期 P99 低 510 ms（−34%），轮间标准差从 M10 的 602 降到 124；逐轮配对 P99 8/10 轮、干扰期 P99 9/10 轮优于轮询。p 值仍是 0.12–0.20（轮询自身的轮间波动很大），10 轮下还不算显著。
    - 两次实验相隔几小时，虚拟机上 ROS 负载不同（轮询组的 P50 从 437 降到 337），所以跨实验只比各自相对轮询的差，不直接比绝对值。
