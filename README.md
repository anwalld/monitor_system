<div align="center">

# linux-monitor

**感知真实负载的 Linux 集群监控与任务调度系统**

内核模块读 CPU · eBPF 测调度延迟 · 多网卡链路质量 · 熔断式节点状态机 · AI 诊断与运维问答

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![C++17](https://img.shields.io/badge/C%2B%2B-17-00599C.svg?logo=cplusplus)](https://isocpp.org/)
[![Linux 6.8](https://img.shields.io/badge/Linux-6.8%20%7C%20eBPF-FCC624.svg?logo=linux&logoColor=black)](https://ebpf.io/)
[![gRPC](https://img.shields.io/badge/gRPC-Protobuf-244c5a.svg)](https://grpc.io/)
[![Python](https://img.shields.io/badge/Python-3.11%20%7C%20FastAPI-3776AB.svg?logo=python&logoColor=white)](ai/)
[![Vue 3](https://img.shields.io/badge/Vue-3%20%7C%20ECharts-4FC08D.svg?logo=vuedotjs&logoColor=white)](web/)

[特性](#特性) · [架构](#架构) · [实测结果](#实测结果) · [快速开始](#快速开始) · [构建](#构建) · [使用](#使用) · [参考](#参考) · [里程碑](#里程碑)

</div>

---

Worker 每秒采集节点指标，Manager 给每台节点打分，把每个任务派给**此刻真正最闲**的节点。有人直接登录节点跑负载，Manager 也看得到。故障节点 3 秒内隔离，恢复后慢启动。AI 服务做异常诊断、运维问答、负载预测和日报，只给建议；AI 整个挂掉，调度照常运行。

## 特性

| | |
|---|---|
| ⚡ **低开销 CPU 采集** | 自写内核模块 `cpustat` 把每核空闲时间写进只读共享页，用户态按 seqlock 协议无锁读取。读取耗时比 `/proc/stat` **降低 99.8%**，加载卸载 1000 次无泄漏 |
| 🔬 **eBPF 调度延迟** | CO-RE 程序统计 run queue 排队延迟直方图，与 bcc `runqlat` 的分布距离 0.04；单核拥挤时 P99 升高约 258 倍，比 CPU 使用率更早反映“排不上队” |
| 🌐 **多网卡链路质量** | 每块网卡用绑定该网卡的原始 ICMP socket 每 200 ms 探测，加上按网卡归类的 TCP 重传率和流量；netem 弱网只让对应网卡变差 |
| 🎯 **概率调度** | 分数 × 在途降权 × 超时衰减 × 慢启动，按权重加权抽样，避免羊群效应；客户端 `Allocate` 拿到地址后直连 Worker，任务数据不经过 Manager |
| 🛡️ **熔断式状态机** | 慢启动 → 正常 → 剔除 / 急救。急救每 1 ms 一条探测，最长 10 秒，反复急救指数退避；进程停止 2.7 秒剔除 |
| 📊 **实时看板** | Vue 3 + ECharts，WebSocket 推送；断线退避重连，按事件 id 补发、去重，Manager 重启后事件不丢不重 |
| 🤖 **AI 旁路** | 稳健 z 分数检测 + LLM 结构化诊断（JSON schema 校验）；六个只读工具的运维问答，同时是 MCP server；日报数字由代码计算，LLM 只写正文并逐个核对 |
| 🧪 **可验证** | GoogleTest 单元测试与 Redis/MySQL 集成测试、ThreadSanitizer 构建、pytest、故障注入评估脚本、四组对照实验 |

## 架构

```
            gRPC Report（每秒）              旁路（挂掉不影响调度）
 ┌────────┐ ───────────────────► ┌─────────┐ ──► Redis（热数据） ──► AI 服务
 │ Worker │                      │ Manager │ ──► MySQL（5 分钟聚合）
 │  ×N    │ ◄─── Probe/Rescue ── │         │ ──► WebSocket ──► 看板
 └────────┘                      └─────────┘
     ▲  任务直连 Submit                ▲ Allocate
     └──────────── 客户端 ─────────────┘
```

- **Worker**（C++）：采集线程每秒出一条样本（CPU 来自 `/dev/cpustat`，调度延迟来自 eBPF，内存、磁盘来自 `/proc`，网卡来自原始 socket 与 Netlink），经环形缓冲交给上报线程；可选的任务服务用 `posix_spawn` 执行白名单命令，子进程不继承 Worker 的能力。
- **Manager**（C++）：接收上报、维护节点表，100 ms 主循环打分、剔除、推进状态机；`Scheduler` 服务负责分配和在途任务账；热数据写 Redis，冷数据由独立线程写 MySQL，看板接口基于 Drogon。
- **AI 服务**（Python）：只读 Redis 和 MySQL，写诊断和日报；负载预测经 `Advisor` 接口推给 Manager（默认关闭）。
- **看板**（Vue 3）：总览、节点详情、事件、诊断、问答、日报、设置，由 Nginx 统一入口。

调度链路上只有 Worker 和 Manager 两方，Redis、MySQL、AI 任何一个挂掉都不影响分配。

## 实测结果

三节点对照实验：虚拟机 lm-worker-1（被另一个项目长期占用）加 Mac 上两个限 2 核的容器，速率 8 个任务/秒，每组 10 轮，每轮随机挑一台节点 `stress` 打满 60 秒。

| 组 | P50 ms | P99 ms | 干扰期 P99 ms | 干扰节点占比降到一半 |
|---|---|---|---|---|
| 轮询 | 437 | 1862 | 2336 | 0/10 次 |
| 加权轮询 | 432 | 2228 | 2392 | 0/10 次 |
| **本系统（不带预测）** | 434 | **1427** | **1773** | **10/10 次，中位 5.8 秒** |
| 本系统（带预测） | 427 | 1706 | 1768 | 10/10 次，中位 6.0 秒 |

本系统每次都在约 6 秒内避开被干扰的节点，P99 低 8%–23%，干扰期 P99 低约 24%。三节点下轮间波动大，统计上还不显著（p 0.15–0.64），负载预测没有带来提升，所以默认不推送。明细见 [`experiments/results/m10-3node/summary.md`](experiments/results/m10-3node/summary.md)。

其他单项验收：

- **故障隔离**：进程停止 2.7 秒剔除；丢弃探测端口的包 2.9 秒剔除；执行超时触发急救，2.2 秒恢复。
- **负载感知**：本地 `stress` 打满时，该节点任务占比从约 60% 降到 0，结束后 10 秒内回升，2464 个任务无失败。
- **存储容错**：MySQL 断开 70 秒，调度不受影响，恢复后一行不少。
- **AI**：16 次故障注入全部检出，延迟 2.5–5 秒；运维问答 20 题全对，平均 1 次工具调用、2.3 秒；停掉 AI 时调度 0 失败。

## 快速开始

**Manager 机**（Docker Compose：Redis、MySQL、Manager、AI 服务、Nginx）：

```bash
git clone https://github.com/anwalld/monitor_system.git && cd monitor_system/deploy
cp .env.example .env      # 填写三个密码
docker compose up -d --build --wait
```

浏览器打开 `http://<manager_ip>/` 就是看板。`.env` 里设了 `API_TOKEN` 时，第一次打开 `http://<manager_ip>/#/?token=<API_TOKEN>`。

**Worker 机**（Ubuntu 24.04，内核 6.8）：

```bash
deploy/check-worker-host.sh                          # 自检：内核、BTF、NO_HZ、网卡
deploy/policy-route.sh                               # 多网卡策略路由
deploy/install-worker.sh -m <manager_ip>:50051 -u ubuntu worker1 worker2 worker3
```

安装脚本会装运行时库，在目标机上编译并加载 `cpustat` 模块，创建 `lm` 用户，写 `/etc/lm/worker.yaml`，用 systemd 启动 `lm-worker`。详细步骤见 [deploy/README.md](deploy/README.md)。

## 构建

依赖：CMake ≥ 3.20、C++17 编译器、gRPC、Protobuf、spdlog、yaml-cpp、hiredis；Manager 另需 mariadb-connector-c 和 Drogon（缺少时 MySQL 写入、看板接口编为桩）；eBPF 需 clang、libbpf、bpftool（仅 Linux，缺少时自动跳过）；看板需 Node.js 22+。redis-plus-plus 和 GoogleTest 找不到时自动下载构建。

<details open>
<summary><b>Ubuntu 24.04</b>（Worker 与 Manager 的目标环境）</summary>

```bash
sudo apt install -y build-essential cmake ninja-build pkg-config \
    libgrpc++-dev protobuf-compiler-grpc libprotobuf-dev protobuf-compiler \
    libspdlog-dev libyaml-cpp-dev libhiredis-dev \
    clang llvm libbpf-dev linux-tools-generic
cmake -S . -B build -G Ninja      # 输出中应有“eBPF：已启用”
cmake --build build
ctest --test-dir build --output-on-failure
```

</details>

<details>
<summary><b>macOS</b>（开发机，能编译和跑单元测试，Worker 只能在 Linux 上运行）</summary>

```bash
brew install cmake ninja grpc protobuf spdlog yaml-cpp hiredis
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH="$(brew --prefix)"
cmake --build build && ctest --test-dir build
```

</details>

<details>
<summary><b>在 macOS 上用容器跑 Linux 构建</b></summary>

```bash
docker build -f deploy/docker/builder.Dockerfile -t lm-builder .
docker run --rm -v "$PWD":/src -w /src lm-builder bash -c \
  'cmake -S . -B build-linux -G Ninja && cmake --build build-linux && ctest --test-dir build-linux'
```

</details>

| CMake 选项 | 默认 | 说明 |
|------|------|------|
| `LM_BUILD_MANAGER` | ON | 编译 Manager |
| `LM_BUILD_TESTS` | ON | 编译单元测试 |
| `LM_ENABLE_TSAN` | OFF | 开启 ThreadSanitizer（用到 gRPC 的测试在该模式下不注册） |
| `LM_ENABLE_BPF` | AUTO | AUTO：Linux 上找到 clang、bpftool、libbpf 和 BTF 时启用；ON：缺少时报错；OFF：不编译 |
| `LM_VMLINUX_BTF` | `/sys/kernel/btf/vmlinux` | 生成 `vmlinux.h` 用的 BTF 文件 |

Redis 集成测试需设置 `LM_TEST_REDIS_URI=tcp://host:6379`（有密码再设 `LM_TEST_REDIS_PASSWORD`），否则跳过。

## 使用

### 单机调试

```bash
make -C kmod/cpustat && sudo insmod kmod/cpustat/cpustat.ko   # 仅 Linux，可选
./build/worker/worker -c config/worker.yaml --dry-run          # 不上报，每秒打印一条 JSON 样本
./build/worker/cpu_read_bench                                  # 共享页 vs /proc/stat 读取耗时
LM_REDIS_PASSWORD=... ./build/manager/manager -c config/manager.yaml
./build/experiments/lm-bench allocate -m 127.0.0.1:50051 -n 1000   # 申请 1000 次，打印各 Worker 被选中比例
./build/experiments/lm-bench submit -m 127.0.0.1:50051 -r 10 -d 60 --token <token>  # 每秒 10 个任务压测
```

### Worker 的自动降级

| 配置项 | 默认 | 行为 |
|---|---|---|
| `cpu_source` | `auto` | 有 `/dev/cpustat` 读共享页，否则回退 `/proc/stat` 并警告；容器内可用 `cgroup` |
| `runq` | `auto` | 能加载 eBPF 就上报调度延迟 P50、P99、丢弃率，否则只警告，样本不带这三个字段 |
| `nic_probe` | `auto` | 按 `net.nics` 配置探测；不配时自动选物理网卡、以默认网关为目标；缺 CAP_NET_RAW 时 `nics` 留空 |

### 查看数据

```bash
redis-cli HGETALL node:<node_id>                # 最新指标、分数明细与剔除状态
redis-cli ZRANGE node:<node_id>:ts -5 -1        # 最近 5 秒样本
redis-cli XREVRANGE stream:events + - COUNT 10  # 最近 10 条事件
```

## 参考

<details>
<summary><b>任务流程与选择权重</b></summary>

```
客户端 ──Allocate──► Manager（选节点，记账 ALLOCATED）
客户端 ──MarkSent──► Manager（SENT，确认计时重新开始）
客户端 ──Submit────► Worker（白名单、token）──OnTaskEvent RECEIVED──► Manager（RUNNING）
                     Worker 执行完 ──────────OnTaskEvent DONE/FAILED──► Manager（销账）
```

确认超时 30 秒（没 MarkSent 的作废，MarkSent 了的计“未确认”），执行超时 300 秒（超时计数 T 加一，按 10 分钟半衰期衰减）。

选择权重 = 分数 × f_inflight（在途量超过自身基线时降权）× f_timeout（exp(−0.5·T)）× f_slow（慢启动）× f_pred（AI 预测，默认 1）。Worker 的任务服务默认关闭，开启方法见 [deploy/README.md](deploy/README.md)。

</details>

<details>
<summary><b>节点状态机</b></summary>

```
新节点 ─► 慢启动 ──5 分钟──► 正常 ──选中概率 < 0.5/N 且 f_timeout 最小──► 急救 ──► 慢启动
            │                 │                                         （失联则剔除）
            └──── 剔除条件 ────┴──► 剔除 ──重新上报、连续 5 次正常、探测成功──► 慢启动
```

- **剔除条件**：3 秒没有上报、关键指标越限、低频探测（每秒一次 `Probe`）连续 3 次失败、最近 5 分钟未确认比例超过全体 3 倍。
- **慢启动**：权重从 0.2 线性升到 1，期间不叠加在途降权。
- **急救**：每 1 ms 一条 `Rescue`，最长 10 秒；健康则快速压低超时计数 T，有失败就下调目标、尽快结束。反复急救按 5、10、20 … 60 分钟退避。

</details>

<details>
<summary><b>看板接口</b></summary>

| 接口 | 内容 |
|------|------|
| `GET /api/nodes`、`/api/nodes/{id}` | 节点实时状态：状态、分数明细、选中概率、CPU、P99、内存、在途、各网卡 |
| `GET /api/nodes/{id}/series?minutes=60&step_s=5` | 最近 1 小时曲线，降采样到每 5 秒一个点（P99 取桶内最大） |
| `GET /api/events?limit=200`、`/api/nodes/{id}/events` | `stream:events` 里的事件，新的在前 |
| `GET /api/health` | 不鉴权，返回 Redis 是否可用 |
| `WS /ws?token=…&since=<事件 id>` | 先补发 since 之后的事件，再发 `snapshot`；之后每秒 `node_update`，事件实时 `event` |

token 放在 `Authorization: Bearer <token>` 或（WebSocket）`?token=`。开发看板：`VITE_MANAGER=http://127.0.0.1:8080 npm --prefix web run dev`。

</details>

<details>
<summary><b>存储</b></summary>

- **Redis（热数据）**：`node:{id}` 最新指标与分数、`node:{id}:ts` 秒级样本、`stream:metrics`、`stream:events`（MAXLEN 约 1 万）。
- **MySQL（冷数据，独立写线程，挂掉不影响调度和 Redis）**：`node` 节点登记；`node_event` 每条事件；`node_metrics_5m` 每节点每 5 分钟一行（按墙上时间对齐）；`task` 任务生命周期，按 1 秒合并写入，超时后迟到的完成会把 `timeout` 改成 `done`。
- **事件种类**：`slow_start`、`normal`、`evict`、`rescue_start`、`rescue_end`、`diagnosis`。

</details>

<details>
<summary><b>AI 服务</b></summary>

AI 服务是独立进程，只读 Redis 和 MySQL，停掉它不影响调度。

- **异常检测**（每秒，纯统计）：每节点每指标 10 分钟窗口，稳健 z 分数 |z| > 4 连续 3 个点算异常，只看变坏方向；`evict`、`rescue_start` 等直接算异常。同一节点 10 分钟只诊断一次，严重度升级除外。
- **LLM 诊断**：把异常、最近 10 分钟指标、状态转移、任务超时、其他节点中位数交给 LLM，按 JSON schema 输出（severity、summary、causes、actions），pydantic 校验，不合格重试一次。只给建议，不执行操作。
- **模型接入**：OpenAI 兼容接口，厂商与模型在 `config/ai.yaml`（DeepSeek、通义千问），看板“设置”页选择并填写 API Key。缺 Key、超出每日 token 预算或调用失败时，诊断只含统计结果。
- **运维问答**：六个只读工具（`list_nodes`、`get_node_metrics`、`get_node_events`、`explain_score`、`get_task_stats`、`get_diagnoses`），看板经 SSE 用函数调用使用；同一套工具也是 MCP server。
- **负载预测**：阻尼趋势的 Holt 线性指数平滑预测 30 秒后的 CPU 忙碌度，换算为 f_pred 推给 Manager。验收时没有优于“当前值当预测”的朴素基线，默认不推送。
- **日报**：每天 09:00 汇总前一天，数字全部由代码从 MySQL 算出；LLM 只写正文，写完逐个核对数字，对不上就只发表格。

| 接口（经 Nginx `/api/ai/`） | 内容 |
|------|------|
| `GET /api/ai/health` | Redis/MySQL 状态、LLM 是否启用、当日 token |
| `GET /api/ai/diagnoses`、`/api/ai/diagnoses/{id}` | 诊断列表、单条诊断（含发给模型的上下文） |
| `GET`/`PUT /api/ai/settings` | 厂商与模型、Key 是否已配置（只返回末 4 位） |
| `POST /api/ai/chat` | 运维问答，SSE 事件 `tool_call`、`tool_result`、`delta`、`done` |
| `GET /api/ai/predictions` | 每节点最新预测与 1 小时 MAE |
| `GET /api/ai/reports`、`POST /api/ai/reports/{day}` | 日报列表、重新生成某天日报 |

MCP server（stdio）接入 Claude Code 等客户端：

```bash
claude mcp add lm-ops -- ssh <manager机> \
  'sudo docker run --rm -i --network host --env-file ~/lm/deploy/.env.mcp lm-ai python -m chat.mcp_server'
```

</details>

<details>
<summary><b>对照实验</b></summary>

`Scheduler.Allocate` 的 `policy` 字段可选 `score`（默认）、`score_nopred`、`rr`（轮询）、`wrr`（按核数的平滑加权轮询）。对照策略只排除失联和探测失败的节点，其余流程相同，四组可以在同一台 Manager 上交替跑。

```bash
python3 experiments/compare_data_server.py --listen 127.0.0.1:18200          # 网络型任务的下载源
python3 experiments/compare.py --out experiments/results/<name> --token "$TOKEN" --node …
python3 experiments/compare_stats.py experiments/results/<name> --md summary.md --json summary.json
```

工作负载为 CPU 密集、IO 密集、网络密集按 2:1:1 混合，同一轮四组用同一种子；指标包括 P50、P99、干扰期 P99、吞吐、失败，以及干扰节点占比降到一半的时间。

</details>

## 目录

```
proto/        gRPC 协议：monitor（上报）、task（任务/探测/急救）、advisor（AI 预测）
kmod/cpustat/ 内核模块：定时把每核空闲时间写进只读共享页 /dev/cpustat
bpf/          eBPF 程序：run queue 排队延迟直方图
common/       环形缓冲、seqlock 读端、配置、日志
worker/       collector/  reporter/  netprobe/  task/  bench/
manager/      ingest/  scoring/  inflight/  breaker/  api/  storage/  scheduler_loop
ai/           AI 服务（FastAPI）：检测、诊断、问答工具、负载预测、日报、MCP server
web/          Vue 3 + Vite + ECharts 看板
experiments/  压测客户端 lm-bench、故障注入与评估脚本、对照实验
tests/        GoogleTest 单元测试与 Redis、MySQL 集成测试
deploy/       docker-compose、MySQL 表结构、策略路由与自检脚本、systemd、安装脚本
openspec/     需求规格与按里程碑划分的变更记录
```

## 里程碑

| 阶段 | 内容 | 验收 |
|------|------|------|
| M0 | 双网卡策略路由、主机自检、Redis/MySQL 编排 | ✅ 单台虚拟机 |
| M1 | proto、/proc 采集、环形缓冲、上报、Manager 写 Redis | ✅ 单台虚拟机 |
| M2 | cpustat 内核模块 + seqlock 共享页 | ✅ 读取耗时 −99.8%，1000 次加载卸载无泄漏 |
| M3 | eBPF 调度延迟直方图 | ✅ 与 bcc runqlat 分布距离 0.04 |
| M4 | 多网卡 ICMP 探测、TCP 重传率、流量 | ✅ netem 弱网只影响对应网卡 |
| M5 | 评分、剔除、概率选择、在途账、任务流程 | ✅ 打满节点占比 60% → 0，2464 任务无失败 |
| M6 | 熔断、急救、慢启动、退避、低频探测 | ✅ 2.7 秒剔除，2.2 秒急救恢复 |
| M7 | 事件流、MySQL 冷数据、REST/WebSocket、看板 | ✅ MySQL 断开 70 秒调度不受影响 |
| M8 | 异常检测、LLM 诊断 | ✅ 16 次注入全部检出 |
| M9 | 运维问答（MCP）、负载预测、日报 | ✅ 问答 20 题全对 |
| M10 | 与轮询、加权轮询对照 | ✅ 三节点完成，待更大规模重跑 |

每个里程碑对应一个 [OpenSpec](https://github.com/Fission-AI/OpenSpec) 变更，归档在 `openspec/changes/archive/`，当前需求规格在 `openspec/specs/`。

**后续计划**：十几台 Worker 规模下重跑对照实验与故障隔离；调度感知 Worker 的并发上限。

## 贡献者

<table>
  <tr>
    <td align="center"><a href="https://github.com/anwalld"><img src="https://github.com/anwalld.png?size=100" width="80" alt="anwalld"/><br/><b>anwalld</b></a><br/>设计、开发、实验</td>
    <td align="center"><a href="https://claude.com/claude-code"><img src="https://github.com/anthropics.png?size=100" width="80" alt="Claude"/><br/><b>Claude</b></a><br/>结对编程（Claude Code）</td>
  </tr>
</table>

## 许可证

[MIT](LICENSE) © 2026 anwalld
