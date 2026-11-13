# 交接文档

更新时间：2026-11-13 14:20（UTC+8），M3 完成之后

## 一句话现状

- **M0**：11 项任务完成 9 项。虚拟机上验证了自检、策略路由和 README 流程。
  - 2.4 只差“经 ens192 访问外网收到应答”：ens192 是 VMware host-only 网络，连不了外网。
  - 4.2 需要 3 台 Worker，目前只有 1 台。
- **M1**：22 项任务完成 21 项。干净 clone 构建通过，虚拟机上的采集、部署、上报和故障恢复都已验证。
  - 4.3 已完成：用户同意把磁盘阈值改为“与内核 io_ticks 误差 ≤ 0.05”，复验最大误差 0.006。
  - 7.1 需要 3 台 Worker。
- **M2**：17 项任务全部完成，虚拟机验收通过（`openspec/changes/m2-cpustat-kmod/`）。
  - 读取耗时：共享页 11.7 ns，/proc/stat 7.7 µs，降低 99.8%。
  - 加载卸载 1000 次无残留；1 毫秒周期跑 10 分钟 dmesg 无警告；重启后模块自动加载。
  - kmemleak 没做：Ubuntu 默认内核未开启。
- **M3**：16 项任务全部完成，虚拟机验收通过（`openspec/changes/m3-ebpf-runqlat/`）。
  - 与 bcc runqlat 对比：负载下总变差距离 0.040、空闲时 0.036，P50、P99 比值都不超过 1.06。
  - 拥挤验收：8 个线程挤在一个核上时，P99 从空闲的 87 µs 升到 22.5 ms。
  - 开销：Worker 空闲 60 秒只用 0.16 秒 CPU；`sched pipe` 极端场景下约慢 5%。
  - 归档顺序：先归档 M1，再归档 M3。
- **提交状态**：M0、M1、M2 已提交并推送（a7fe517、b99badf、790061f）。M3 的改动还在工作区，没有 commit，提交前先问用户。

## 2026-11-13 这次做了什么

- 2:25 的定时任务卡在第一条 Bash 的权限确认上，什么也没做。这次改由主会话手动完成。
- **M1 6.3**：用 `git ls-files` 打包后在全新的 ubuntu:24.04 容器里按 README 构建，所有测试都通过。
- **虚拟机 M0**：
  - 1.2 通过。
  - 2.4 部分通过：规则、幂等、出口网卡（tcpdump 确认）、ssh 不断、`--down` 恢复都没问题，只有 host-only 网卡无外网应答这一项不行。
  - 4.1 通过，lm-policy-route 单元测完后已删除。
- **虚拟机 M1**：
  - install-worker.sh 部署成功。
  - 虚拟机访问不了 Docker Hub，改为从 Mac 用 `docker save | ssh docker load` 把 redis、mysql、lm-manager 三个镜像传过去（方法已写进 deploy/README.md）。
  - compose（项目 lm）在虚拟机上运行。
  - 7.2 通过。
- **修复：Worker 重连太慢**。gRPC 默认重连退避会长到数十秒，Manager 重启后 26 秒才恢复上报，改成最多 1 秒后实测 4.8 秒恢复。这些实现中的决定已补进 m1 的 design.md 第 9 节。

### 虚拟机当前状态（保留着，方便继续测）

- **systemd**：`lm-worker` 在运行（M3 版本：读 cpustat 共享页，eBPF 调度延迟已启用），二进制在 `/opt/lm/bin/worker`，配置在 `/etc/lm/worker.yaml`，`manager_addr` 是 `127.0.0.1:50051`。
- **cpustat 模块**：源码在 `/opt/lm/kmod/cpustat`，模块装在 `/lib/modules/6.8.0-149-generic/extra/`，由 `/etc/modules-load.d/cpustat.conf` 开机加载，udev 规则 `/etc/udev/rules.d/99-cpustat.rules`。
- **docker compose**：项目 `lm` 在 `~/lm/deploy`（manager 镜像已更新为 M3 版本，会写 runq 字段），跑着 redis、mysql、manager。密码是随机生成的，在虚拟机的 `~/lm/deploy/.env` 里。
- **已装的软件**：stress、fio、docker.io、docker-compose-v2、Worker 运行时库（含 libbpf1）、build-essential、linux-headers、bpfcc-tools（runqlat-bpfcc）、linux-tools（perf、bpftool）。
- **策略路由**：已撤销，`ip rule` 恢复原样。`/opt/lm/deploy/policy-route.conf` 里写的是 `ens160` 和 `ens192 172.16.13.1`。

## 关键链接与环境

| 项 | 值 |
|----|----|
| 开发文档（权威需求） | https://claude.ai/artifact/7gFfa9w6pxbuwo7WM1EYhN （docs 工具读取，正文较长，读成纯文本再看） |
| GitHub | git@github.com:anwalld/monitor_system.git（用户自己的公开仓库，历史已清成单个初始提交） |
| Ubuntu 虚拟机 | `ssh ubuntu@172.16.255.133`，见下表 |
| OpenSpec 变更 | `openspec/changes/m0-environment/`、`openspec/changes/m1-skeleton/`，项目约定在 `openspec/config.yaml` |
| Linux 构建镜像 | `lm-builder`（`deploy/docker/builder.Dockerfile`，Ubuntu 24.04 aarch64），产物在 `build-linux/` |

Ubuntu 虚拟机（`ssh ubuntu@172.16.255.133`）：

| 项 | 值 |
|----|----|
| 系统 | Ubuntu 24.04.5，内核 6.8.0-149，**aarch64**，4 核 |
| 网卡 | ens160 172.16.255.133/24（ssh 走这块）、ens192 172.16.13.128/24 |
| 权限 | sudo 免密，ssh 密钥登录可用 |
| 数量 | 目前只有 1 台。文档要求 3 台 Worker 的验收项只能部分完成，要如实标注 |

因为虚拟机也是 aarch64，`lm-builder` 容器里编出的 `build-linux/worker/worker` 可以直接拷过去运行。

## 已完成

### M0（11 项任务完成 7 项）

`deploy/` 下的文件：

- `check-worker-host.sh`：Worker 主机自检
- `policy-route.sh`：多网卡策略路由，附 `policy-route.conf.example`
- `systemd/lm-policy-route.service`
- `docker-compose.yml`：redis、mysql、manager 三个服务
- `.env.example`
- `mysql/init.sql`：六张表
- `README.md`：搭建与验收步骤

验证情况：

- **策略路由**：在特权容器里用 dummy 网卡验证过，包括规则、幂等、推断失败不改动、`--down` 恢复、rp_filter 保存与恢复。
- **compose**：30 秒内 healthy，表结构正确，down 再 up 后数据还在，缺密码时拒绝启动。
- **一个坑**：`.env.example` 原来写了行内注释，会被当成密码，已改为注释单独一行。

### M1（22 项任务完成 18 项）

代码：

- `proto/`：三个 proto 文件（package lm）
- `common/`：`lm::Ring` 环形缓冲、`Config`、日志与 `RateLimiter`、时钟
- `worker/collector/`：`CpuSource` 接口、`ProcCpuSource`、`CpuBusyCalculator`、内存与磁盘读取、`Collector` 线程
- `worker/reporter/`：`Reporter` 线程
- `worker/main.cpp`：支持 `--dry-run`
- `manager/ingest/`：`NodeTable`、`CollectorService`
- `manager/storage/`：`BoundedQueue`、`RedisWriter`
- `manager/main.cpp`

部署相关：

- `deploy/docker/manager.Dockerfile`：多阶段构建
- compose 里的 manager 服务：host 网络
- `deploy/systemd/lm-worker.service`
- `deploy/install-worker.sh`：会检查二进制与目标机架构是否一致
- 配置样例：`config/worker.yaml`、`config/manager.yaml`
- `README.md`：已按新架构重写

实现中做的、design.md 里没写的决定（后续可以补进 design）：

1. **Ring 只返回更新的样本**：`take_latest` 只返回比上次取到的更新的样本，所以读到的 seq 严格递增。
2. **Reporter 错开半拍**：Reporter 首拍延迟半个周期（`Options::phase`）。原因是采集与上报周期相同、同时启动时，节拍抖动会导致漏报，实测 12 条只报了 8 条，修复后报 10 条（首尾两条属正常损失）。
3. **redis-plus-plus 从源码构建**：Ubuntu 24.04 和 Homebrew 都没有这个包，CMake 用 FetchContent 构建，并手动补了生成头文件的路径和 hiredis 链接。
4. **TSan 下跳过 gRPC 测试**：系统包的 gRPC 和 absl 没用 TSan 编译，误报很多。所以 TSan 构建不注册用到 gRPC 的测试（`test_worker_rpc`、`test_manager_rpc`），其余测试都在 TSan 下跑。抑制规则在 configure 时按实际库自动生成。
5. **Redis 地址可用环境变量覆盖**：Manager 支持用 `LM_REDIS_URI` 覆盖配置。compose 里用它指向 `127.0.0.1:${REDIS_PORT}`。

### 刚修掉、还没复验的问题

- 旧 `.gitignore` 里有一行 `cmake/`，会让 `cmake/grpc.cmake` 不进仓库，干净 clone 后无法构建。这一行已删除，但还没复验（见下一步第 1 条）。

## 下一步（按顺序）

1. **等用户决定**：
   - 是否提交并推送 M3。不要自行 commit 或 push。
   - 是否把虚拟机第二块网卡从 host-only 改成 NAT 或桥接（改了才能完整验收 2.4），以及是否再加 2 台 Worker（4.2 和 7.1 要 3 台）。
2. **用户加了虚拟机后**：用 `install-worker.sh -m <manager>:50051 -u ubuntu <ip...>` 部署，补测 M0 4.2 和 M1 7.1。每台都要先跑 `check-worker-host.sh`，再配策略路由。
3. **M4 规划**：用 openspec-propose 生成 `m4-multi-nic`。内容按开发文档“多网卡链路质量”一节：
   - 每块网卡 ICMP 探测：epoll + timerfd 线程，RTT、抖动、超时率。
   - Netlink tcp_info 重传率。
   - sysfs 收发流量。
   - 填 `Sample.nics`。
   - 验收：netem 注入后只有对应网卡的指标变差。
   - 注意：虚拟机的 ens192 是 host-only，连不了外网，探测目标可以用网关 172.16.13.1。
4. **之后按里程碑表推进**：M5 调度、M6 节点状态机……

## 注意事项

- **定时任务**：无人值守运行时会卡在权限确认上。下次设定时任务前，先提醒用户在应用里给任务设置权限模式，或者先点一次“Run now”把要用的命令批准掉。
- 用户习惯：中文、简短、异步。能自己决定的就决定，把决定写进 design.md 后汇报，不要因为小问题停下来等。
- **虚拟机计时器**：arm64 计时器一跳约 41 ns，测纳秒级耗时要分批计时（见 m2 design 第 11 节）。
- **worker 日志在 stderr**：M3 起所有组件的日志写 stderr，`--dry-run` 的 stdout 是纯 JSON。
- **后台命令里的 `wait`**：脚本里有后台 stress 时，`wait` 会连它一起等，导致卡住。要用 `wait <pid>`。
- **macOS 上的命令**：zsh 不会把 `$VAR` 按空格拆成多个参数，需要多个参数时用 bash 脚本文件。
- **docker exec 吃标准输入**：在 heredoc 里调用 `docker exec` 会读走后面的脚本，要加 `</dev/null` 或改用脚本文件。
- **清理测试容器**：`lmtest-*` 系列容器、卷、网络用完都要删掉。用户自己的 compose 项目名是 `lm`，不要动。
- 旧 git 历史的备份在旧 scratchpad 里：`/private/tmp/claude-501/-Users-mima0000--claude-spares-spare-QCgCC7/aac8d5c9-c883-49f8-81c7-591bfb84aeb4/scratchpad/monitor_system_old_history.bundle`（临时目录，可能已被清理）。
