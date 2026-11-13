# 交接文档

更新时间：2026-11-13 18:50（UTC+8）。M4 已提交并推送；M5（m5a + m5b）已实现并在虚拟机验收，**改动尚未提交**。

## 现状

| 里程碑 | OpenSpec 变更 | 任务 | 提交 | 说明 |
|---|---|---|---|---|
| M0 环境 | `m0-environment` | 9/11 | a7fe517 | 剩 2.4（第二块网卡要能上外网）、4.2（要 3 台 Worker） |
| M1 骨架 | `m1-skeleton` | 21/22 | b99badf | 剩 7.1（要 3 台 Worker） |
| M2 内核模块 | `m2-cpustat-kmod` | 17/17 | 790061f | 虚拟机验收通过 |
| M3 eBPF | `m3-ebpf-runqlat` | 16/16 | 1c64e22 | 虚拟机验收通过 |
| M4 多网卡 | `m4-multi-nic` | 15/15 | 4c22841 | 虚拟机验收通过 |
| M5 评分与选择 | `m5a-scoring` | 9/9 | 未提交 | 虚拟机验收通过 |
| M5 任务流程 | `m5b-task-flow` | 12/12 | 未提交 | 虚拟机 + Mac 测试容器验收通过；3 台 Worker 到位后重做 5.4 |

- M5 的改动都在工作区里，没有提交（等用户开口）。建议一个提交包含 m5a 和 m5b（或各一个）。提交前检查别带上 `build-*`、`.env`。
- 七个变更都还没有归档（`openspec/changes/archive/` 为空），`openspec/specs/` 也是空的。
- **归档顺序有依赖**：M3 的 `manager-hot-storage` 增量依赖 M1 引入的能力，所以要先归档 M1，再归档 M3。M0 和 M1 有未完成的任务，归档前先问用户。

## 关键验收数据（详细记录在各变更的 tasks.md 里）

- **M1**：
  - Manager 重启后 4.8 秒恢复上报。原来要 26 秒，修复方法是把 gRPC 重连退避上限压到 1 秒。
  - Redis 恢复后 0.39 秒，节点数据重新开始更新。
  - 磁盘利用率与内核 io_ticks 算出的值最大相差 0.006。阈值已按用户同意改为“误差 ≤ 0.05”。
- **M2**：
  - 共享页读一次 11.7 ns，`/proc/stat` 7.7 µs，耗时降低 99.8%。
  - 加载卸载 1000 次无残留；1 毫秒周期跑 10 分钟无内核警告。
  - kmemleak 没测：Ubuntu 默认内核没开这个功能。
- **M3**：
  - 与 bcc runqlat 对比，总变差距离：负载下 0.040，空闲时 0.036。
  - 8 个线程挤在一个核上时，P99 从空闲的 87 µs 升到 22.5 ms。
- **M4**：
  - 给 ens192 注入 netem（丢包 10%、延迟 100ms）后，ens192 的 RTT 升高 100 ms、重传率 0.113；ens160 不受影响。
  - 流量与 `/proc/net/dev` 相差 0.03%。
- **开销**：三项采集全开时，空闲 60 秒 Worker 用 0.35 秒 CPU。
- **M5a**：stress -c 4 时第 3 条满载样本后剔除（reason cpu），Allocate 返回 RESOURCE_EXHAUSTED；结束后第 5 条正常样本恢复。
- **M5b**：两台节点（lm-worker-1 + Mac 上的测试容器）10 个/秒 sleep 0.2：
  - stress -c 4 期间 lm-worker-1 占比从约 0.64 降到 0，结束后 5–10 秒回升；stress -c 2 期间 0.61 → 0.52。两组 2464 个任务无失败，P99 213 ms。
  - Worker 被 SIGKILL 丢事件 → 300 秒后执行超时 3 次，f_timeout 0.2235；正常 restart 不丢事件。
  - 任务子进程 CapEff/CapAmb 为 0。

## 下一步

1. **等用户确认后提交 M5**（m5a-scoring、m5b-task-flow），然后按顺序归档（M1 → M3 → 其余；M0、M1 有未完成任务，归档前先问）。
2. **M6 节点状态机**：熔断器（正常/剔除/急救/慢启动）、急救双向流（`WorkerTask.Rescue`，1ms 探测，Worker 改内核刷新周期）、慢启动 f_slow、指数退避、低频探测（`Probe` 已在 m5b 实现）、灰色故障剔除（用 m5b 的“未确认”计数）。挂在 `manager/scheduler_loop.cpp` 上。验收 = 故障注入表里节点类故障全部符合期望，需要至少 2 台 Worker：可以沿用 Mac 上的 Docker 测试 Worker（见 m5b design 第 11 节），但“节点宕机/恢复”最好用真虚拟机。
3. **等用户决定**：
   - 是否把虚拟机第二块网卡改成 NAT 或桥接，以完成 M0 2.4。
   - 是否再加 2 台 Worker，以完成 M0 4.2、M1 7.1，并重做 M5b 5.4、做 M6 验收。
4. **后续里程碑**：M7 存储与看板、M8–M9 AI、M10 对照实验。

## 环境

| 项 | 值 |
|----|----|
| 开发文档（权威需求） | https://claude.ai/artifact/7gFfa9w6pxbuwo7WM1EYhN 。用 docs 工具 export 成 markdown，返回 JSON 里的 `bytes_b64` 解码后就是正文。最近一次解码的副本在 scratchpad 的 `devdoc.md`（临时目录，可能已被清理） |
| GitHub | git@github.com:anwalld/monitor_system.git（用户自己的公开仓库） |
| 项目约定 | `openspec/config.yaml` |
| Linux 构建镜像 | `lm-builder`（`deploy/docker/builder.Dockerfile`，Ubuntu 24.04 aarch64，含 clang、libbpf、bpftool）。产物在 `build-linux/`；TSan 构建在 `build-tsan/`（不含 Manager）和 `build-tsan-m/`（含 Manager，M5 起用这个查 Manager 并发） |
| Ubuntu 虚拟机 | `ssh ubuntu@172.16.255.133`，见下 |

常用命令（M5 起还有 `build/experiments/lm-bench allocate|submit`，见 README）：

```bash
docker run --rm -v "$PWD":/src -w /src lm-builder bash -c 'cmake -S . -B build-linux -G Ninja && cmake --build build-linux && ctest --test-dir build-linux'
deploy/install-worker.sh -m 127.0.0.1:50051 -u ubuntu 172.16.255.133      # 部署 Worker 和 cpustat 模块到虚拟机
docker build -f deploy/docker/manager.Dockerfile -t lm-manager . && docker save lm-manager | gzip -1 | ssh ubuntu@172.16.255.133 'gunzip | sudo docker load'
```

### 虚拟机 lm-worker-1

- **系统**：Ubuntu 24.04.5，内核 6.8.0-149，aarch64，4 核。sudo 免密，ssh 用密钥登录。和构建镜像是同一架构，容器里编出的二进制可以直接拷过去用。
- **网卡**：
  - ens160：172.16.255.133，NAT，ssh 走这块，网关 172.16.255.2。
  - ens192：172.16.13.128，VMware host-only，只能到 Mac 宿主机 172.16.13.1。
- **访问不了 Docker Hub**：镜像要从 Mac 用 `docker save | ssh docker load` 传过去。
- **Worker**：systemd 单元 `lm-worker`（用户 lm），运行的是 M5 版本（旧配置备份在 `/etc/lm/worker.yaml.m4.bak`）：
  - CPU 读 cpustat 共享页。
  - eBPF 调度延迟采集已启用。
  - 网卡探测已启用。`/etc/lm/worker.yaml` 末尾手工加了 `net.nics`：ens160 → 172.16.255.2、8.8.8.8；ens192 → 172.16.13.1。
  - `manager_addr` 改成了 172.16.255.133:50051（原来是 127.0.0.1，Manager 会把回环地址当作 Worker 地址发给客户端）。
  - 任务服务已开启：白名单 /bin/sleep、/usr/bin/stress、/bin/grep、/bin/sh，随机 token 在配置文件里（640 root:lm）。Mac 上取 token：`ssh ubuntu@172.16.255.133 'sudo grep token: /etc/lm/worker.yaml'`。
- **cpustat 模块**：源码在 `/opt/lm/kmod/cpustat`，模块在 `/lib/modules/6.8.0-149-generic/extra/`。开机由 `/etc/modules-load.d/cpustat.conf` 加载，udev 规则是 `/etc/udev/rules.d/99-cpustat.rules`。
- **compose 项目 `lm`**：在 `~/lm/deploy`，跑着 redis、mysql、manager（M5 版本镜像；回滚用的旧镜像标签 `lm-manager:m3`、`lm-manager:m5a`）。密码是随机生成的，在 `~/lm/deploy/.env`。
- **策略路由**：目前未启用。`/opt/lm/deploy/policy-route.conf` 写的是 `ens160` 和 `ens192 172.16.13.1`。
- **已装的软件**：stress、fio、docker、Worker 运行时库（含 libbpf1）、build-essential、linux-headers、bpfcc-tools（runqlat-bpfcc）、linux-tools（perf、bpftool）。

## 注意事项

**和用户协作**
- 用户用中文，消息简短，异步协作。能自己决定的就决定，把决定写进 design.md 后再汇报，不要因为小问题停下来等。
- commit 和 push 要等用户开口。每个里程碑单独一个提交，提交前检查有没有把 `.env`、`build-*`、`vmlinux.h`、skeleton 头文件带进去。
- 定时任务无人值守时会卡在权限确认上。下次设定时任务前，先提醒用户配好权限。

**虚拟机上测试**
- 不能弄断 ens160 的 ssh。改路由或流量控制之前，先用 `systemd-run --on-active=Nmin` 设好自动撤销。
- 经 ens192 的 TCP 测试只能连 Mac。可以在 Mac 上临时起一个只监听 172.16.13.1 的接收端，测完就停。
- 8.8.8.8 经 NAT 本身约丢 11%。用它作为探测目标时，判断网卡是否劣化要看变化量，不能看绝对值。
- arm64 计时器一跳约 41 ns，测纳秒级耗时要分批计时。

**M5 测试**
- 第二个节点：Mac 上 `docker run -d --name lmtest-worker2 -p 50052:50052 -v "$PWD/build-linux":/b:ro -v <worker2.yaml>:/w.yaml:ro lm-builder /b/worker/worker -c /w.yaml`，配置 `cpu_source: proc`、`runq: off`、`nic_probe: off`、`manager_addr: 172.16.255.133:50051`、task 段与虚拟机相同 token。Manager 看到的地址是 172.16.255.1:50052。测完删掉。
- `lm-bench allocate -n N` 会在 Manager 账上留下 N 个 ALLOCATED 任务，30 秒后才作废，期间会压低 f_inflight；做占比测试前先等 30 秒。
- 8.8.8.8 丢包让 ens160 网络分偏低，所以 lm-worker-1 满分约 0.92，不是 1.0。

**数据与输出格式**
- proto 的 JSON 输出里，uint64 字段是字符串，比如 `rx_bps`、`tx_bps`、`seq`，要先转成数字再算。
- 所有组件的日志都写 stderr，`worker --dry-run` 的 stdout 是纯 JSON。

**macOS 与 Docker**
- zsh 不会把 `$VAR` 按空格拆成多个参数，需要多个参数时改用 bash 脚本文件。
- 在 heredoc 里调用 `docker exec` 会读走后面的脚本，要加 `</dev/null`。
- 脚本里有后台进程时，要用 `wait <pid>`。直接 `wait` 会连后台的 stress 也一起等，导致卡住。
- 测试用的容器、卷、网络统一用 `lmtest-` 前缀，用完删掉。用户自己的 compose 项目叫 `lm`，不要动。
- 本地 `build/` 里如果出现重复注册的测试（ctest 数量翻倍），删掉 `build/tests/*_include.cmake` 和 `*_tests.cmake` 后重新 configure。

**其他**
- 旧 git 历史的备份：`/private/tmp/claude-501/-Users-mima0000--claude-spares-spare-QCgCC7/aac8d5c9-c883-49f8-81c7-591bfb84aeb4/scratchpad/monitor_system_old_history.bundle`（32 MB，在临时目录里，随时可能被清理）。
