# linux-monitor

[![License](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)
[![C++](https://img.shields.io/badge/C%2B%2B-17-blue.svg)](https://isocpp.org/)
[![Platform](https://img.shields.io/badge/platform-Linux-lightgrey.svg)](https://www.linux.org/)

Linux 服务器性能监控与调度系统。Worker 采集节点指标，Manager 给每台节点打分，把每个任务派给此刻最合适的节点。AI 服务负责异常诊断、运维问答、负载预测和日报，Web 看板实时展示。

> 正在按里程碑重构，目前完成到 M6：采集 → 上报 → Manager → Redis 的链路已打通，CPU 读数来自 cpustat 内核模块的共享页，调度延迟由 eBPF 测量，每块网卡单独测链路质量和流量；Manager 按这些指标给节点打分、剔除越限节点，客户端 `Allocate` 拿到 Worker 地址后直连 Worker 提交任务，Manager 用在途任务账跟踪每个任务；每台节点有熔断器式的状态（慢启动、正常、剔除、急救），故障节点 3 秒内隔离，恢复后慢启动。进度见下文“里程碑”。

## 目标

- **派给最闲的节点**：任务派给当前最闲的节点。别人直接登录节点跑的负载，Manager 也看得到。
- **故障隔离**：故障节点 3 秒内隔离，恢复后自动慢启动、重新接任务。
- **低开销采集**：CPU 读取耗时比 `/proc/stat` 低 85% 以上，撑得住 1ms 级的急救探测。
- **AI 只给建议**：AI 服务整个挂掉，调度照常运行。

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

调度链路上只有 Worker 和 Manager 两方。客户端从 Manager 拿到 Worker 地址后，直连 Worker 提交任务，任务数据不经过 Manager。

## 目录

```
proto/        monitor.proto（上报）  task.proto（任务/探测）  advisor.proto（AI 预测）
kmod/cpustat/ 内核模块：定时把每核空闲时间写进只读共享页（/dev/cpustat）
bpf/          eBPF 程序：runqlat.bpf.c（调度排队延迟直方图），构建时生成 vmlinux.h 和 skeleton
common/       环形缓冲、seqlock 读端、配置、日志
worker/       collector/（CPU、内存、磁盘）  reporter/（每秒上报）  netprobe/（多网卡 ICMP 探测、TCP 重传率、流量）  task/（任务执行服务）  bench/（CPU 读取耗时基准、调度延迟对照工具）  main.cpp
manager/      ingest/（接收上报、节点表）  scoring/（评分、剔除、加权抽样）  inflight/（在途任务账）  breaker/（节点状态机、低频探测、急救）  api/（Scheduler 服务、看板 REST/WebSocket）  scheduler_loop（100 ms 主循环）  storage/（Redis 热数据、事件流、MySQL 冷数据）  main.cpp
ai/           AI 服务（Python 3.11、FastAPI）：lmai/（检测、诊断、LLM 接入、接口）  tests/（pytest）
web/          Vue 3 + Vite + ECharts 看板（总览、节点详情、事件、诊断、设置）
experiments/  lm-bench：调试与压测客户端（allocate、submit）；ws_check.mjs：看板断线重连验收；ai_faults.sh、ai_eval.py：AI 检测的故障注入与评估；mock_llm.py：模拟 OpenAI 兼容接口
tests/        GoogleTest 单元测试与 Redis、MySQL 集成测试
config/       worker.yaml、manager.yaml 样例
deploy/       docker-compose、MySQL 表结构、策略路由与自检脚本、systemd、安装脚本
openspec/     需求规格与按里程碑划分的变更
```

后续里程碑会加入 `ai/`。

## 构建

依赖：

- CMake ≥ 3.20、C++17 编译器
- gRPC、Protobuf、spdlog、yaml-cpp
- hiredis、MySQL C 客户端（mariadb-connector-c）、Drogon（Manager 需要；后两者缺少时 MySQL 写入、看板接口编为桩）
- Node.js 22+（构建看板 `web/`）
- clang、libbpf-dev、bpftool（Worker 的 eBPF 调度延迟采集需要，仅 Linux；缺少时自动跳过）

redis-plus-plus 和 GoogleTest 找不到时，CMake 会自动下载源码构建。

### Ubuntu 24.04（Worker 与 Manager 的目标环境）

```bash
sudo apt install -y build-essential cmake ninja-build pkg-config \
    libgrpc++-dev protobuf-compiler-grpc libprotobuf-dev protobuf-compiler \
    libspdlog-dev libyaml-cpp-dev libhiredis-dev \
    clang llvm libbpf-dev linux-tools-generic
cmake -S . -B build -G Ninja      # 输出中应有“eBPF：已启用”
cmake --build build
ctest --test-dir build --output-on-failure
```

### macOS（开发机）

```bash
brew install cmake ninja grpc protobuf spdlog yaml-cpp hiredis
cmake -S . -B build -G Ninja -DCMAKE_PREFIX_PATH="$(brew --prefix)"
cmake --build build && ctest --test-dir build
```

macOS 上能编译、能跑单元测试（不含 eBPF），但 Worker 依赖 `/proc`，只能在 Linux 上运行。

### 在 macOS 上用容器跑 Linux 构建与测试

```bash
docker build -f deploy/docker/builder.Dockerfile -t lm-builder .
docker run --rm -v "$PWD":/src -w /src lm-builder bash -c \
  'cmake -S . -B build-linux -G Ninja && cmake --build build-linux && ctest --test-dir build-linux'
```

### CMake 选项

| 选项 | 默认 | 说明 |
|------|------|------|
| `LM_BUILD_MANAGER` | ON | 编译 Manager |
| `LM_BUILD_TESTS` | ON | 编译单元测试 |
| `LM_ENABLE_TSAN` | OFF | 开启 ThreadSanitizer。用到 gRPC 的测试在该模式下不注册 |
| `LM_ENABLE_BPF` | AUTO | 编译 eBPF 程序。AUTO：Linux 上找到 clang、bpftool、libbpf 和 BTF 时启用；ON：缺少时报错；OFF：不编译 |
| `LM_VMLINUX_BTF` | `/sys/kernel/btf/vmlinux` | 生成 `vmlinux.h` 用的 BTF 文件 |

Redis 集成测试需要设置环境变量 `LM_TEST_REDIS_URI=tcp://host:6379`（有密码时再设 `LM_TEST_REDIS_PASSWORD`），否则跳过。

## 运行

搭建环境的详细步骤见 [deploy/README.md](deploy/README.md)。

### Manager 机

```bash
cd deploy
cp .env.example .env      # 填写三个密码
docker compose up -d --build --wait
```

compose 会启动 Redis、MySQL、Manager、AI 服务和 Nginx。Manager 用 host 网络，gRPC 监听 50051，看板接口监听 8080；AI 服务只监听 127.0.0.1:8000；Nginx 监听 80（`HTTP_PORT`），浏览器打开 `http://<manager_ip>/` 就是看板。`.env` 里设了 `API_TOKEN` 时，第一次打开 `http://<manager_ip>/#/?token=<API_TOKEN>`，之后浏览器会记住。

### Worker 机

第一步，每台 Worker 机先做两件事：

1. 运行 `deploy/check-worker-host.sh` 自检。
2. 用 `deploy/policy-route.sh` 配好多网卡策略路由。

第二步，在一台与 Worker 同架构的 Linux 上构建出 `build-linux/worker/worker`，然后批量安装：

```bash
deploy/install-worker.sh -m <manager_ip>:50051 -u ubuntu worker1 worker2 worker3
```

安装脚本会做这些事：

- 装好运行时库、编译工具和当前内核的头文件
- 在目标机上编译 cpustat 内核模块，设为开机加载，并用 udev 规则让 `lm` 组可读 `/dev/cpustat`
- 创建系统用户 `lm`
- 写入 `/etc/lm/worker.yaml`，其中 `node_id` 留空，表示使用主机名
- 用 systemd 启动 `lm-worker`

Worker 的 `runq` 默认是 `auto`：能加载 eBPF 程序就每秒上报调度延迟 P50、P99 和丢弃率，加载不了（缺 BTF、缺 CAP_BPF）就只记一条警告，样本里不带这三个字段。

Worker 的 `nic_probe` 默认是 `auto`：每块网卡用绑定该网卡的原始 ICMP socket 每 200ms 探测一次目标，加上按网卡归类的 TCP 重传率和流量，填进样本的 `nics`。目标在 `net.nics` 里配置（见 `config/worker.yaml`），不配时自动选物理网卡并以默认网关为目标。缺少 CAP_NET_RAW 时只警告，`nics` 留空。

Worker 的 `cpu_source` 默认是 `auto`：有 `/dev/cpustat` 就读共享页，没有（模块没加载、内核升级后没重编）就回退到 `/proc/stat` 并记一条警告。

### 单机调试

```bash
make -C kmod/cpustat && sudo insmod kmod/cpustat/cpustat.ko   # 仅 Linux：编译并加载内核模块（可选）
./build/worker/worker -c config/worker.yaml --dry-run    # 不上报，每秒把样本以 JSON 打印出来
./build/worker/cpu_read_bench                            # 对比共享页与 /proc/stat 的读取耗时
LM_REDIS_PASSWORD=... ./build/manager/manager -c config/manager.yaml
./build/experiments/lm-bench allocate -m 127.0.0.1:50051          # 申请一次节点，打印 task_id 和 Worker 地址
./build/experiments/lm-bench allocate -m 127.0.0.1:50051 -n 1000  # 申请 1000 次，打印每台 Worker 被选中的比例
./build/experiments/lm-bench submit -m 127.0.0.1:50051 -r 10 -d 60 --token <token>  # 每秒 10 个 sleep 0.2，打印各节点占比和完成时间
```

### 任务流程

```
客户端 ──Allocate──► Manager（选节点，记账 ALLOCATED）
客户端 ──MarkSent──► Manager（SENT，确认计时重新开始）
客户端 ──Submit────► Worker（白名单、token）──OnTaskEvent RECEIVED──► Manager（RUNNING）
                     Worker 执行完 ──────────OnTaskEvent DONE/FAILED──► Manager（销账）
```

确认超时 30 秒（没 MarkSent 的作废，MarkSent 了的计“未确认”），执行超时 300 秒（超时计数 T 加一，按 10 分钟半衰期衰减）。选择权重 = 分数 × f_inflight（在途量超过自身基线时降权）× f_timeout（exp(−0.5·T)）× f_slow（慢启动）。Worker 的任务服务默认关闭，开启方法见 `deploy/README.md`。

### 节点状态

```
新节点 ─► 慢启动 ──5 分钟──► 正常 ──选中概率 < 0.5/N 且 f_timeout 最小──► 急救 ──► 慢启动
            │                 │                                         （失联则剔除）
            └──── 剔除条件 ────┴──► 剔除 ──重新上报、连续 5 次正常、探测成功──► 慢启动
```

- 剔除条件：3 秒没有上报、关键指标越限、低频探测（每秒一次 `Probe`）连续 3 次失败、最近 5 分钟未确认比例超过全体 3 倍。
- 慢启动：权重从 0.2 线性升到 1，期间不叠加在途降权。
- 急救：每 1 ms 一条 `Rescue` 请求，最长 10 秒；健康则快速压低超时计数 T，有失败就下调目标、尽快结束。反复急救按 5、10、20 … 60 分钟退避。

### 看板与接口（M7）

| 接口 | 内容 |
|------|------|
| `GET /api/nodes`、`/api/nodes/{id}` | 节点实时状态（来自 Manager 内存）：状态、分数明细、选中概率、CPU、P99、内存、在途、各网卡 |
| `GET /api/nodes/{id}/series?minutes=60&step_s=5` | 最近 1 小时曲线，降采样到每 5 秒一个点（P99 取桶内最大） |
| `GET /api/events?limit=200`、`/api/nodes/{id}/events` | `stream:events` 里的事件，新的在前 |
| `GET /api/health` | 不鉴权，返回 Redis 是否可用 |
| `WS /ws?token=…&since=<事件 id>` | 先补发 since 之后的事件，再发 `snapshot`；之后每秒 `node_update`，事件实时 `event` |

token 放在 `Authorization: Bearer <token>` 或（WebSocket）`?token=`。看板断线后按 1、2、4…30 秒退避重连，带上收到的最大事件 id，按 id 去重。开发看板：`VITE_MANAGER=http://127.0.0.1:8080 npm --prefix web run dev`。验收断线重连：

```bash
node experiments/ws_check.mjs --url http://<manager_ip> --token <token> --close-after 5 --during '<制造事件的命令>'
```

### 冷数据（M7）

Manager 另起一个 MySQL 写线程，MySQL 挂掉不影响调度和 Redis：

- `node`：新节点登记；`node_event`：每条事件（detail 里的 `event_id` 就是 `stream:events` 的 entry id）。
- `node_metrics_5m`：每节点每 5 分钟一行（按墙上时间对齐，桶结束 5 秒后写入；退出时写出未结束的桶）。
- `task`：分配、MarkSent、收到、完成、超时、作废按 1 秒合并后写入；超时后迟到的完成会把 `timeout` 改成 `done`。

事件种类：`slow_start`（新节点 / 剔除后 / 急救后）、`normal`、`evict`、`rescue_start`、`rescue_end`，写入 `stream:events`（MAXLEN 约 1 万）。

### AI 诊断（M8）

AI 服务是独立进程，只读 Redis 和 MySQL，写 `ai_diagnosis` 和 `stream:events` 里的 `diagnosis` 事件。停掉它不影响调度。

- **检测**（每秒，纯统计）：每台节点每个指标（CPU、调度延迟 P99、可用内存、磁盘、各网卡 RTT/超时率/重传率）保留 10 分钟窗口，稳健 z 分数 |z| > 4 连续 3 个点算异常，只看变坏的方向；调度延迟丢弃率 > 1%、网卡超时率 > 20%（10 秒平均）、`evict`、`rescue_start` 直接算异常。同一节点 10 分钟只诊断一次，严重度升级除外。
- **诊断**：把异常、最近 10 分钟指标（每 10 秒一个点）、状态转移、任务超时、其他节点中位数交给 LLM，按 JSON schema 输出（severity、summary、causes、actions），pydantic 校验，不合格重试一次。诊断只给建议，不执行任何操作。
- **LLM**：OpenAI 兼容接口，厂商和模型列在 `config/ai.yaml`（DeepSeek、通义千问），在看板“设置”页选择；API Key 直接在设置页填写（存 Redis `ai:keys`，不回显），或放 `deploy/.env`，设置页填的优先。没选厂商、缺 Key、超出每日 token 预算（默认 100 万）或调用失败时，诊断只含统计结果。

| 接口（经 Nginx `/api/ai/`） | 内容 |
|------|------|
| `GET /api/ai/health` | 不鉴权：Redis/MySQL 状态、LLM 是否启用、当日 token、检测节点数、队列 |
| `GET /api/ai/diagnoses?node=&severity=&hours=24&limit=100` | 诊断列表，新的在前 |
| `GET /api/ai/diagnoses/{id}` | 一条诊断的全部内容（含发给模型的上下文） |
| `GET /api/ai/anomalies?since_ms=` | 内存里最近 1000 个检测器异常（含被去重的），评估用 |
| `GET`/`PUT /api/ai/settings` | 可选厂商与模型、Key 是否已配置（只返回来源和末 4 位）；PUT `{"vendor","model","api_key"?,"clear_key"?}`，vendor 为空表示不启用 |

开发：`docker build -f deploy/docker/ai.Dockerfile -t lm-ai . && docker run --rm lm-ai python -m pytest -q`；本机联调用 `python3 experiments/mock_llm.py` 模拟 LLM。看板开发时 `VITE_AI` 指向 AI 服务（默认 `http://127.0.0.1:8000`）。

### 查看数据

```bash
redis-cli HGETALL node:<node_id>                         # 最新指标、分数明细（score、s_cpu、s_runq、s_mem、s_net、best_nic）与剔除状态（evicted、evict_reason）
redis-cli ZRANGE node:<node_id>:ts -5 -1                  # 最近 5 秒样本
redis-cli XREVRANGE stream:metrics + - COUNT 1            # 最近一秒所有节点
redis-cli XREVRANGE stream:events + - COUNT 10            # 最近 10 条事件
```

## 里程碑

| 阶段 | 内容 | 状态 |
|------|------|------|
| M0 环境 | 双网卡策略路由、主机自检、Redis/MySQL 编排 | 单台虚拟机验收完成；待第二块网卡能上外网、3 台 Worker |
| M1 骨架 | proto、/proc 采集、环形缓冲、上报、Manager 写 Redis | 单台虚拟机验收完成；待 3 台 Worker |
| M2 内核模块 | cpustat 共享页 + seqlock，替换 /proc 读 CPU | 虚拟机验收完成：读取耗时降低 99.8%，加载卸载 1000 次无泄漏 |
| M3 eBPF | 调度延迟直方图 | 虚拟机验收完成：与 bcc runqlat 分布距离 0.04，单核拥挤时 P99 升高约 258 倍 |
| M4 多网卡 | ICMP 探测、TCP 重传率、流量 | 虚拟机验收完成：netem 弱网只让对应网卡变差（RTT +100ms、重传 11%），另一块不受影响 |
| M5 调度 | 评分、剔除、概率选择、在途账、任务流程、压测客户端 | 验收完成（1 台虚拟机 + Mac 上 1 个测试用 Worker 容器）：本地 stress 打满时该节点任务占比从约 60% 降到 0，结束后 10 秒内回升，2464 个任务无失败；待 3 台 Worker 重做 |
| M6 节点状态机 | 熔断、急救、慢启动、退避、低频探测 | 验收完成（1 台虚拟机 + Mac 上 1 个测试用 Worker 容器）：进程停止 2.7 秒剔除不急救；恢复后进入慢启动、5 分钟回到正常；丢 50052 端口的包 2.9 秒剔除；执行超时触发自动急救，2.2 秒恢复；间歇故障时急救 0.5 秒内发现并结束 |
| M7 存储与看板 | 事件流、MySQL 冷数据、REST/WebSocket、Vue 看板、Nginx | 验收完成（1 台虚拟机 + Mac 上 1 个测试用 Worker 容器）：每节点每 5 分钟一行（300 样本），超时后迟到的完成改为 done；MySQL 断开 70 秒调度不受影响、恢复后一行不少；看板断线和 Manager 重启后事件不丢不重 |
| M8 AI 第一批 | 异常检测、LLM 诊断、诊断页与设置页（BYOK） | 验收完成（虚拟机 + Mac 测试节点）：16 次注入全部检出，延迟 2.5–5 秒；DeepSeek 诊断 JSON 全部合格；停 AI、阻断 MySQL 时调度 0 失败 |
| M9 AI 第二批 | 运维问答（MCP）、负载预测、日报 | |
| M10 对照实验 | 与轮询、加权轮询对比 | |

每个里程碑对应 `openspec/changes/` 下的一个变更，开发流程见 [OpenSpec](https://github.com/Fission-AI/OpenSpec)。

## 许可证

[MIT](LICENSE)
