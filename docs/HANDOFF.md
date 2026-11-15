# 交接文档

更新时间：2026-11-15（UTC+8）。M0–M8 已提交并推送。虚拟机 compose 里跑着 M7 的 Manager、M8 的 AI 服务和看板（经用户同意于 16:57 并入）。下一个里程碑是 M9（AI 第二批）。

## 现状

| 里程碑 | OpenSpec 变更 | 任务 | 提交 | 说明 |
|---|---|---|---|---|
| M0 环境 | `m0-environment` | 9/11 | a7fe517 | 剩 2.4（第二块网卡要能上外网）、4.2（要 3 台 Worker） |
| M1 骨架 | `m1-skeleton` | 21/22 | b99badf | 剩 7.1（要 3 台 Worker） |
| M2 内核模块 | `m2-cpustat-kmod` | 17/17 | 790061f | 虚拟机验收通过 |
| M3 eBPF | `m3-ebpf-runqlat` | 16/16 | 1c64e22 | 虚拟机验收通过 |
| M4 多网卡 | `m4-multi-nic` | 15/15 | 4c22841 | 虚拟机验收通过 |
| M5 评分与选择 | `m5a-scoring` | 9/9 | 441c7a8 | 虚拟机验收通过 |
| M5 任务流程 | `m5b-task-flow` | 12/12 | 441c7a8 | 虚拟机 + Mac 测试容器验收；3 台 Worker 到位后重做 5.4 |
| M6 节点状态机 | `m6-node-state-machine` | 16/16 | 5170274 | 虚拟机 + Mac 测试容器验收；3 台 Worker 到位后用真关机重做 4.3、4.4 |
| M7 存储与事件 | `m7a-storage-events` | 10/10 | c13de6e | 虚拟机 + Mac 测试容器验收 |
| M7 看板 | `m7b-dashboard` | 8/8 | c13de6e | 虚拟机 + Mac 测试容器验收 |
| M8 AI 第一批 | `m8-ai-diagnosis` | 12/12 | 见 git log | 虚拟机 + Mac 测试节点验收；DeepSeek 真实诊断 |

- 十一个变更都没有归档（`openspec/changes/archive/` 为空，`openspec/specs/` 也是空的）。归档顺序有依赖：M3 的 `manager-hot-storage` 增量依赖 M1 引入的能力，要先归档 M1 再归档 M3；M8 对 `web-dashboard` 用了 MODIFIED，要先归档 M7b 再归档 M8。M0、M1 有未完成任务，归档前先问用户。
- 每个变更的实测数据在各自 `tasks.md`，实现中改过的方案在各自 `design.md` 的「实现中的决定」。

## 下一步

1. **M9 AI 第二批**（运维问答 + MCP、负载预测 + `PushPredictions`、日报）。按开发文档“2. 自然语言运维问答”“3. 负载预测参与调度”“4. 每日报告”规划。已有：AI 服务框架（`ai/lmai`，FastAPI、Redis/MySQL 访问、LLM 客户端与预算、看板设置页）；`ai_daily_report` 表已建；`proto/advisor.proto` 已有 `PushPredictions`。
   - 问答要 Manager 把分数明细写进 `node:{id}`（已有 s_cpu 等）；`POST /api/ai/chat` 走 SSE，nginx `/api/ai/` 的 `proxy_read_timeout` 和缓冲要调。
2. **等用户决定**：第二块网卡能否上外网（M0 2.4）；是否再加 2 台 Worker（M0 4.2、M1 7.1、M5b 5.4、M6 4.3/4.4）。
3. **归档**：见上。
4. 之后：M10（对照实验）。

## 关键验收数据

- **M1**：Manager 重启后 4.8 秒恢复上报（gRPC 重连退避上限压到 1 秒，原来 26 秒）；Redis 恢复后 0.39 秒数据重新更新；磁盘利用率与 io_ticks 最大差 0.006（阈值经用户同意为 ≤ 0.05）。
- **M2**：共享页读一次 11.7 ns，`/proc/stat` 7.7 µs，降低 99.8%；加载卸载 1000 次无残留；1 ms 周期跑 10 分钟无内核警告。kmemleak 没测（Ubuntu 默认内核没开）。
- **M3**：与 bcc runqlat 的总变差距离：负载 0.040、空闲 0.036；8 线程挤一个核时 P99 从 87 µs 升到 22.5 ms。
- **M4**：ens192 注入 netem（丢包 10%、延迟 100 ms）后 RTT +100 ms、重传率 0.113，ens160 不受影响；流量与 `/proc/net/dev` 差 0.03%。三项采集全开，空闲 60 秒 Worker 用 0.35 秒 CPU。
- **M5a**：stress -c 4 时第 3 条满载样本后剔除（cpu），Allocate 返回 RESOURCE_EXHAUSTED；结束后第 5 条正常样本恢复。
- **M5b**：两节点 10 个/秒 sleep 0.2：stress -c 4 期间 lm-worker-1 占比约 0.64 → 0，结束后 5–10 秒回升；stress -c 2 时 0.61 → 0.52；2464 个任务无失败，P99 213 ms。Worker 被 SIGKILL 丢事件 → 300 秒后执行超时 3 次，f_timeout 0.2235。任务子进程 CapEff/CapAmb 为 0。
- **M6**：Worker 进程停止 2.7 秒剔除（probe 先于 lost），不急救；恢复 5.7 秒进入慢启动，5 分钟后正常；丢 50052 包 2.9 秒剔除、删规则 1.6 秒恢复；执行超时触发自动急救 2.2 秒结束（T 8 → 0.87）；间歇故障急救 0.5 秒结束。
- **M7**：每节点每 5 分钟一行，连续桶 300 样本（跑了 23.5 小时，282 行）；310 秒的任务 300 秒记执行超时、迟到完成后 task 表为 done；事件 id 在 `stream:events` 与 `node_event` 一致；MySQL 断开 70 秒期间 228 个任务全部完成、恢复后一行不少（验收中修复了断开时首批丢失）；看板主动断开、Manager 重启后不丢不重，浏览器 1 秒后重连。

- **M8**：16 次注入全部检出：测试节点 cpu 4.0–4.7 s、mem 4.1–4.6 s、disk 4.6–4.8 s、nic 3.4–4.5 s、stop 2.7–3.0 s；虚拟机 runq 4.2/4.9 s（ROS 背景下一次 46.8 s）。DeepSeek（deepseek-chat）调用 JSON 全部合格，约 5000 token/次；停止故障一度被归因成网卡，上下文加“上报情况”后正确。压测中停 AI 60 秒、阻断 MySQL 60 秒，任务 0 失败，P99 约 213 ms。空闲 1 小时误报：干净测试节点 0 次；虚拟机在 ROS 负载下 32 次（全部是真实负载变化）。

## 环境

| 项 | 值 |
|----|----|
| 开发文档（权威需求） | https://claude.ai/artifact/7gFfa9w6pxbuwo7WM1EYhN 。用 docs 工具 export 成 markdown，返回 JSON 的 `bytes_b64` 解码后是正文。上次解码的副本：`~/Backups/monitor_system/devdoc.md`（11-13 导出，文档更新后需重新导出） |
| GitHub | git@github.com:anwalld/monitor_system.git（用户的公开仓库） |
| 项目约定 | `openspec/config.yaml` |
| Linux 构建镜像 | `lm-builder`（`deploy/docker/builder.Dockerfile`，Ubuntu 24.04 aarch64，含 clang、libbpf、bpftool、mariadb、Drogon）。产物在 `build-linux/`；TSan 在 `build-tsan/`（不含 Manager）和 `build-tsan-m/`（含 Manager） |
| 镜像 | `lm-manager`（Manager）、`lm-web`（看板 + Nginx），都在 Mac 上构建后传到虚拟机 |

### 虚拟机 lm-worker-1（`ssh ubuntu@172.16.255.133`）

- **系统**：Ubuntu 24.04.5，内核 6.8.0-149，aarch64，4 核 8 GB。sudo 免密，密钥登录。与构建镜像同架构，容器里编出的二进制可直接用。
- **网卡**：ens160 172.16.255.133（NAT，ssh 走这块，网关 172.16.255.2）；ens192 172.16.13.128（VMware host-only，只能到 Mac 172.16.13.1）。策略路由未启用（`/opt/lm/deploy/policy-route.conf` 已写好）。
- **访问不了 Docker Hub**：镜像用 `docker save | ssh 'sudo docker load'` 传。
- **Worker**：systemd 单元 `lm-worker`（用户 lm），M6 版本：
  - CPU 读 cpustat 共享页；eBPF 调度延迟已启用；网卡探测已启用（`/etc/lm/worker.yaml` 末尾手工加了 `net.nics`：ens160 → 172.16.255.2、8.8.8.8；ens192 → 172.16.13.1）。
  - `manager_addr: 172.16.255.133:50051`（不能用 127.0.0.1，Manager 会把回环地址当 Worker 地址发给客户端）。
  - 任务服务开启：白名单 /bin/sleep、/usr/bin/stress、/bin/grep、/bin/sh；随机 token 在配置文件里（640 root:lm），取法 `ssh ubuntu@172.16.255.133 'sudo grep token: /etc/lm/worker.yaml'`。
- **cpustat 模块**：源码 `/opt/lm/kmod/cpustat`，模块在 `/lib/modules/6.8.0-149-generic/extra/`，开机由 `/etc/modules-load.d/cpustat.conf` 加载，udev 规则 `/etc/udev/rules.d/99-cpustat.rules`。
- **compose 项目 `lm`**（`~/lm/deploy`）：redis、mysql、manager（M7）、ai（M8，`lm-ai:latest` = `lm-ai:m8`，正式配置去重 600 秒，监听 127.0.0.1:8000）、nginx（`lm-web:m8`，看板，80 端口，http://172.16.255.133/ ，`/api/ai/` → AI）。
  - 密码在 `~/lm/deploy/.env`（随机生成）；没有 `API_TOKEN`，看板接口不鉴权。
  - `docker-compose.yml` 是 M8 版本；M7 版本备份为 `docker-compose.yml.m7.bak`，M6 版本为 `docker-compose.yml.m6.bak`。看板回滚标签 `lm-web:m7`。
  - LLM 选的是 DeepSeek（deepseek-chat），Key 由用户在看板设置页填写（Redis `ai:keys`）；每日 token 预算 100 万（用户 2026-11-15 从 20 万调高），ROS 负载下 lm-worker-1 大约每 10 分钟诊断一次、每次约 5000 token，一天约 70 万。
  - Manager 回滚标签：`lm-manager:m6`、`m5`、`m5a`、`m3`。
- **虚拟机上的 ROS 仿真**：2026-11-15 15:03 起用户在 `~/robot_ws` 跑 ROS 2 Jazzy + Gazebo，会长期运行；CPU 常在 0.9 以上、可用内存 0.3–0.5，Manager 会反复剔除 lm-worker-1，AI 大约每 10 分钟诊断一次 lm-worker-1（每次约 5000 token）。不要动这些进程；需要干净节点时用 Mac 上的 `lmtest-worker2`。
- **已装软件**：stress、fio、docker、Worker 运行库（含 libbpf1）、build-essential、linux-headers、bpfcc-tools（runqlat-bpfcc）、linux-tools（perf、bpftool）。

## 常用操作

```bash
# Linux 容器里构建与测试（集成测试的环境变量见下）
docker run --rm -v "$PWD":/src -w /src lm-builder bash -c 'cmake -S . -B build-linux -G Ninja && cmake --build build-linux && ctest --test-dir build-linux'

# 部署 Worker 和 cpustat 模块到虚拟机
deploy/install-worker.sh -m 127.0.0.1:50051 -u ubuntu 172.16.255.133

# 构建并部署 Manager、AI、看板（先留回滚标签：ssh … 'sudo docker tag lm-manager:latest lm-manager:<旧版本>'）
docker build -f deploy/docker/manager.Dockerfile -t lm-manager .
docker build -f deploy/docker/web.Dockerfile -t lm-web .
docker build -f deploy/docker/ai.Dockerfile -t lm-ai .
docker save lm-manager lm-ai lm-web | gzip -1 | ssh ubuntu@172.16.255.133 'gunzip | sudo docker load'
ssh ubuntu@172.16.255.133 'cd ~/lm/deploy && sudo docker compose up -d --no-build manager ai nginx'

# AI 检测验收（干净节点 lmtest-worker2，见下）与统计
experiments/ai_faults.sh -c lmtest-worker2 -n lmtest-worker2 -i eth1 -o f.jsonl cpu mem disk nic stop
python3 experiments/ai_eval.py --url http://172.16.255.133 --faults f.jsonl

# 压测客户端（见 README）、看板断线重连验收
./build/experiments/lm-bench submit -m 172.16.255.133:50051 -r 10 -d 60 --token <token>
node experiments/ws_check.mjs --url http://172.16.255.133 --close-after 5 --during '<制造事件的命令>'
```

**测试环境**（测试用的容器、卷、网络一律 `lmtest-` 前缀，用完删掉）：

- **第二个节点**：Mac 上 `docker run -d --name lmtest-worker2 -p 50052:50052 -v "$PWD/build-linux":/b:ro -v <worker2.yaml>:/w.yaml:ro lm-builder /b/worker/worker -c /w.yaml`。配置自己写：`node_id: lmtest-worker2`、`cpu_source: proc`、`runq: off`、`nic_probe: off`、`manager_addr: 172.16.255.133:50051`、task 段与虚拟机相同（含 token）。Manager 看到的地址是 172.16.255.1:50052。
- **Redis / MySQL 集成测试**：Mac 上起 `lmtest-redis`（redis:7-alpine）和 `lmtest-mysql`（mysql:8.0，`-e MYSQL_DATABASE=lm -e MYSQL_USER=lm -e MYSQL_PASSWORD=…`，挂 `deploy/mysql/init.sql` 到 `/docker-entrypoint-initdb.d/`），设 `LM_TEST_REDIS_URI`、`LM_TEST_MYSQL_HOST/PORT/PASSWORD`；Linux 容器里主机名用 `host.docker.internal`。不设这些变量时集成测试自动跳过。
- **本机跑 Manager 与看板**：Mac 的 8080 被别的项目（cppaiserver-gateway）占着，18090 也被占用；本机配置把 `api.listen` 改成 18080。看板开发 `VITE_MANAGER=http://127.0.0.1:18080 npm --prefix web run dev`。
- **干净的测试节点（M8）**：`lmtest-worker2` 用镜像 `lmtest-w2img`（lm-builder + iproute2、fio、ping），两块网卡（默认 bridge 的 eth0 + `lmtest-net2` 10.213.0.0/24 的 eth1），`--cap-add NET_ADMIN`，`nic_probe: on`（eth0 → 172.17.0.1，eth1 → 10.213.0.1）。注入：`experiments/ai_faults.sh -c lmtest-worker2 -n lmtest-worker2 -i eth1 cpu mem disk nic stop`；统计：`experiments/ai_eval.py --faults <jsonl>`。同一 Docker 虚拟机里用户的 cppaiserver 容器在 M8 验收时按用户要求停了（`docker start $(docker ps -aq --filter label=com.docker.compose.project=cppaiserver)` 恢复）。
- **AI 服务本机联调**：`lmtest-redis`、`lmtest-mysql` + `python3 experiments/mock_llm.py` + `experiments/replay_metrics.py`（合成样本）；镜像内测试 `docker run --rm lm-ai python -m pytest -q`。
- **用浏览器看虚拟机上的看板**：内置浏览器只能开本机页面，先 `ssh -f -N -L 127.0.0.1:18091:127.0.0.1:80 ubuntu@172.16.255.133`，再开 http://localhost:18091 。

## 注意事项

**和用户协作**
- 用户用中文，消息简短，异步协作。能自己决定的就决定，写进 design.md 后再汇报，不为小问题停下来等。
- 2026-11-14 用户授权“一直做，不用问”：里程碑验收完直接提交并推送，每个里程碑一个提交。提交前检查别带上 `.env`、`build-*`、`vmlinux.h`、skeleton 头文件、`web/node_modules`、`web/dist`、`.claude/launch.json`（本机开发配置）。
- 仍要先问：删用户数据；动 compose 项目 `lm` 里除换 Manager 镜像以外的东西（停 MySQL、改 `.env` 等）；需要用户账号或密钥的事（如 M8 的 LLM Key）。
- 定时任务无人值守时会卡在权限确认上，设定时任务前先提醒用户配好权限。

**虚拟机上测试**
- 不能弄断 ens160 的 ssh。改路由、流量控制、iptables 之前，先用 `systemd-run --on-active=Nmin` 设好自动撤销。
- 模拟 MySQL 故障不停容器：iptables 丢回环口 3306（`-I INPUT -i lo -p tcp --dport 3306 -j DROP`，先设自动撤销）。
- 经 ens192 的 TCP 测试只能连 Mac，可以在 Mac 上临时起只监听 172.16.13.1 的接收端，测完就停。
- 8.8.8.8 经 NAT 本身约丢 11%，判断网卡劣化要看变化量；这也让 lm-worker-1 满分约 0.92 而不是 1.0。
- arm64 计时器一跳约 41 ns，测纳秒级耗时要分批计时。
- 虚拟机会随 Mac 休眠或关机停掉；开机后 Worker 和 compose 自动起来，但所有节点重新慢启动 5 分钟。

**调度与状态机测试**
- Manager 每秒探测 Worker 的 50052，测试 Worker 必须开任务服务。
- Manager 重启后所有节点慢启动 5 分钟，急救只从 normal 触发，要等回到 normal 再测。
- 造 T：暂停测试 Worker（`docker pause`）让它被剔除，向 lm-worker-1 提交比执行超时长的任务（`--args 400 --timeout 400`），300 秒后记执行超时。Worker 并发上限 8。
- 间歇故障脚本参考 m6 tasks 4.6。
- `lm-bench allocate -n N` 会留下 N 个 ALLOCATED 任务，30 秒后才作废，期间压低 f_inflight；做占比测试前先等 30 秒。

**AI 服务（M8）**
- 检测器的最近异常（`http://172.16.255.133/api/ai/anomalies`）和去重状态只在内存里，重启 AI 容器就清空；评估前先存下来（`ai_eval.py --file`）。模拟 LLM 要用时自己起：`python3 experiments/mock_llm.py`，并在配置里加 mock 厂商（见 m8 tasks 1.3）。
- `worker.yaml` 里的 token 带引号，取出来给 lm-bench 用要 `tr -d '"'`，否则全部 UNAUTHENTICATED。
- LLM 去重：同一节点 10 分钟只诊断一次，严重度升级除外；想让每次注入都有诊断，测试配置里临时把 `dedup_s` 调小。
- 容器里的 `/proc` 是 Docker 虚拟机全局的：在任何容器里跑 stress 都会体现在测试节点的指标上。

**数据与格式**
- proto 的 JSON 输出里 uint64 字段是字符串（`rx_bps`、`tx_bps`、`seq`），要先转数字。
- 所有组件的日志写 stderr，`worker --dry-run` 的 stdout 是纯 JSON。
- 5 分钟聚合按 Manager 收到样本时的墙上时间分桶：开机时钟回拨会让某个桶样本数超过 300；Manager 在同一桶内重启时后写的覆盖先写的。

**macOS、Docker 与构建**
- zsh 不会把 `$VAR` 按空格拆成多个参数，需要时改用 bash 脚本文件。
- heredoc 里调用 `docker exec` 会读走后面的脚本，要加 `</dev/null`。
- 脚本里有后台进程时用 `wait <pid>`，直接 `wait` 会连后台的 stress 一起等。
- 本地 `build/` 出现重复注册的测试（ctest 数量翻倍）时，删掉 `build/tests/*_include.cmake` 和 `*_tests.cmake` 后重新 configure。
- ports.ubuntu.com 偶尔 502、很慢，重建 `lm-builder` 或 Manager 镜像可能要 20 分钟以上，失败就重试。
- Ubuntu 的 `libdrogon-dev` 缺开发依赖（jsoncpp、uuid、zlib、pq、sqlite3、brotli、mariadb 的 `-dev`），Dockerfile 里已显式安装；MySQL 客户端因此统一用 mariadb-connector-c。
- 旧 git 历史备份：`~/Backups/monitor_system/monitor_system_old_history.bundle`（32 MB，已校验完整；用 `git clone` 或 `git fetch` 从 bundle 取回）。
