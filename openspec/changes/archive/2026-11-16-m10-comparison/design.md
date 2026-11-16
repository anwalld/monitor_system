# Design

## Context

- 现有节点：lm-worker-1（虚拟机，4 核，用户的 ROS 仿真长期占 CPU，节点经常被剔除）和 Mac Docker 虚拟机里的测试容器（原来只有 lmtest-worker2，`/proc` 是整台 Docker 虚拟机的 10 核；实验时改成两个限 2 核的容器，见决定 11 和「实现中的决定」2）。节点差异大，正好是“核数不同 + 背景负载不同”的异构场景。
- Manager、AI、Redis、MySQL 在虚拟机 compose 项目 `lm` 里；压测客户端在 Mac 上跑。
- M9 的预测推送默认关闭（没跑赢朴素基线）。

## Goals / Non-Goals

- Goals：四组共用同一 Manager、同一组 Worker、同一任务序列；切组不重启 Manager（避免慢启动 5 分钟）；工具与节点数无关，以后加机器直接重跑。
- Non-Goals：在三个节点上得出“普遍成立”的结论；改评分默认参数。

## Decisions

1. **对照策略放在 Manager，按请求选择**（而不是在客户端自己轮询）。
   - 理由：四组都经过 Allocate → MarkSent → 事件 → 在途账，Manager 侧的超时统计、任务表、看板对四组一视同仁；切组只是换一个请求字段，不用重启 Manager 或换镜像，各组可以交替运行。
   - 代价：proto 多一个字段；生产环境的客户端理论上也能选对照策略，用 `allocation.allow_policy` 可关掉。
2. **对照组的健康检查**：`fresh`（3 秒内有上报）且 `probe_fail_streak` 未达到剔除线。不看关键指标剔除和分数——那正是本系统要证明的价值；但完全不做健康检查的轮询在现实中不存在，会把差异夸大。
3. **轮询顺序**：候选按 `node_id` 排序，Manager 内一个计数器取模；候选集合变化时不重置计数器。**加权轮询**用 nginx 的平滑加权轮询（每次各候选 `current += weight`，选最大者并减去总权重），权重为上报的 `ncpu`（为 0 时按 1）；状态在 Manager 内按 `node_id` 保存，节点离开候选时清掉。都在节点表锁内完成，不做 IO。
4. **`score_nopred`** 复用 `select_locked`，只把 `f_pred` 置 1；`score`（默认）不变。
5. **预测推送**：实验期间整段打开（临时镜像 `lm-ai:m10push`，在 `lm-ai` 上改 `/etc/lm/ai.yaml` 的 `push`），`score_nopred` 在 Manager 侧忽略它，所以不用中途换镜像。实验结束换回 `lm-ai:latest`（推送关闭）。
6. **任务类型**：
   - CPU：`/bin/sh -c` 跑固定次数的 shell 循环（空闲时约 0.3 秒），完成时间随争用变长。
   - IO：`dd if=/dev/zero of=/tmp/lm-io-$LM_TASK_ID bs=1M count=16 oflag=direct` 后删除。
   - 网络：`python3` 从 `http://{host}:18200/4m` 下载 4 MB。虚拟机经 ens192 到 Mac（172.16.13.1），容器经 Docker 到 Mac（host.docker.internal）。Worker 白名单只有 `/bin/sh` 等，全部任务都用 `/bin/sh -c` 包一层。
   - 比例 CPU:IO:网络 = 2:1:1，类型用种子 `seed + 1000003` 的独立随机流抽取，保证到达时刻序列与以前同种子一致。
7. **干扰**：每次运行一个 CPU 干扰（`stress -c <核数>`，持续 60 秒，开始于运行的 30%–50% 之间），节点和时刻由运行种子决定，同一轮的四组完全相同。
   - 测试容器改成 `--cpus 2` + `cpu_source: cgroup`（见决定 11），stress 在容器里以正常优先级跑，只吃这 2 核，不影响同一 Docker 虚拟机上的 cppaiserver。
   - 网卡干扰（netem）做成可选（`--net-fault`，默认关）：评分的网络项取“最好的网卡”，lm-worker-1 和 lmtest-worker2 都各有一块不走数据面的网卡（ens160、lmtest-net2 那块），劣化数据面网卡不改变任何组的选择，只会给四组加同样的噪声。虚拟机上开启时 netem 打在 ens192（不碰 ssh 走的 ens160），先用 `systemd-run --on-active` 设好自动撤销。iperf 型干扰同理不做。
8. **交替运行**：第 r 轮的组顺序为基础顺序左移 r 位，抵消时间漂移（ROS 负载、Docker 虚拟机负载随时间变）。每次运行前等 35 秒（ALLOCATED 任务 30 秒作废、干扰残留消失），并等节点都不在急救中。
9. **反应时间**：CPU 干扰开始前 30 秒内该节点的分配占比为基线 b；干扰开始后，以 5 秒滑动窗口计算占比，第一次 ≤ b/2 的窗口终点减去干扰开始时刻即反应时间；60 秒内没降到一半记为“未降”（统计时按 60 秒上限另列次数）。基线占比太小（b < 0.1，或窗口内分配数 < 10）的运行不计反应时间。
10. **显著性**：每组 10 轮的 P50、P99 取均值和标准差；两组差值与合并标准差之比（Cohen's d）和 Welch t 检验的 p 值一起报告。不用 scipy（Mac 上 python 3.9 无第三方包），t 分布 p 值用正态近似并注明。
11. **测试容器限核与 cgroup 来源**：lmtest-worker2 原来读 Docker 虚拟机全局的 `/proc`（10 核，含其他容器的负载）。容器里低优先级的 stress 拖不慢任务却让指标变差，正常优先级又会占满整台 Docker 虚拟机。改成 `docker run --cpus 2`，Worker 用 `cpu_source: cgroup`（usage_usec / 配额），任务并发上限 4（2 × 核数，与虚拟机默认公式一致）。内存、磁盘仍读全局，不进评分的磁盘无影响，内存只是展示偏差。

## Risks / Trade-offs

- **节点少**：三个节点里本系统能做的只是“少给忙的那台”；节点多时选择空间更大，差异会不同。报告注明。
- **ROS 背景负载**：lm-worker-1 常被关键指标剔除，本系统组几乎全给 lmtest-worker2；这既是本系统的优势场景，也让“带/不带预测”难以区分（剔除先于预测起作用）。
- **共享 Docker 虚拟机**：容器节点的 CPU 指标包含 cppaiserver 等其他容器，网络任务的下载源也在同一台 Mac 上。
- **压测客户端在 Mac 上**：Mac 自身负载会进完成时间，四组受影响相同。

## 实现中的决定

1. **第一次正式实验作废（2026-11-16 13:31–15:07，两节点，跑了 15/40 次）**：
   - lm-worker-1 上用户的 ROS 仿真在 14 点前后加重（load 8–12，4 核），节点长期接近剔除线。CPU 干扰一打到 lmtest-worker2，两台节点同时被剔除，`score`、`score_nopred` 组整段 60 秒 Allocate 返回 RESOURCE_EXHAUSTED（每次 400–700 个失败），比较的其实是“ROS 负载 + 两节点”而不是调度。
   - 14:22 起 Mac 休眠约 16 分钟（会话空闲后防休眠失效；macOS 的单调时钟在休眠时停走），r3 的三次运行被打断。
   - 处理：结果删除；实验进程改用 `caffeinate -i` 包住；监视脚本对每次运行超过 300 秒报警。
2. **改成三节点**：加 `lmtest-worker3`（同样 `--cpus 2`、`cpu_source: cgroup`、并发上限 4、`--init` 回收 stress 子进程）。两个容器经 Docker Desktop 出站时对端 IP 相同（Manager 用“对端 IP + 50052”拼地址会冲突），所以 worker3 的 `manager_addr` 用虚拟机的 host-only 地址 172.16.13.128，Manager 看到 172.16.13.1；端口分别只发布在 172.16.255.1:50052 和 172.16.13.1:50052。lm-worker-1 保留，作为“被别人的负载长期占着的真实节点”。
3. **容器 PID 1 不回收僵尸**：`docker exec … stress` 被 pkill 后子进程变成僵尸挂在 Worker（PID 1）下，重建容器时加 `--init`。
4. **本系统组失败数高于轮询（实验结论之一，未修）**：失败几乎都是 Worker 并发上限拒收。节点少、其中一台长期高负载时，本系统把流量集中到剩下的节点上，而评分不知道 Worker 的并发上限。修复方向见 tasks 4.3 的建议，留给后续变更，本变更按 proposal 不改调度参数。
