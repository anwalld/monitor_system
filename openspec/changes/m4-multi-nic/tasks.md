# Tasks

## 1. 纯逻辑模块（worker/netprobe，各平台都编）

- [x] 1.1 `lm::Config` 增加 `node(key)`；实现 `nic_config` 的解析与校验（网卡存在性通过可注入的检查函数判断，便于测试；目标 1–3 个合法 IPv4；interval ≥ 50、timeout ≥ interval）。验证：gtest 覆盖正常配置、不存在的网卡（错误信息含网卡名）、非法地址、目标超过 3 个、缺省参数
  > 2026-11-13 完成：`NetConfig.*` 覆盖正常配置、默认值、不存在的网卡（错误信息含 eth9）、非法地址、超过 3 个目标、空目标、interval 与 timeout 校验。
- [x] 1.2 实现 `icmp`：构造回显请求（identifier、seq、16 字节数据）、RFC 1071 校验和、解析带 IPv4 头的回显应答（类型、源地址、identifier、网卡编号、seq）。验证：gtest 用手工构造的报文覆盖正确应答、错误类型、错误 identifier、错误网卡编号、截断报文，并用已知向量校验校验和
  > 完成：`Icmp.*` 用 RFC 1071 示例校验校验和，覆盖构造后解析、错误类型、截断、校验和错误、非 ICMP 协议。
- [x] 1.3 实现 `probe_state`：待确认队列、成功匹配、队头超时、迟到回包丢弃、发送失败计超时、周期汇总并清零。验证：gtest 覆盖规格例子（9 次成功 1..9 ms 加 1 次超时 → 均值 5、抖动约 2.582、超时率 0.1）、连续 3 秒不回应 → 超时率 1、1.2 秒迟到只计一次超时、空周期全 0
  > 完成：`ProbeState.*` 覆盖规格例子（均值 5、抖动 2.582、超时率 0.1）、不可达时全超时、迟到回包只计一次超时、来源不对被拒、发送失败计超时、乱序回包按 seq 匹配。
- [x] 1.4 实现 `retrans`（`RetransTracker`）和 `traffic`（`RateCounter`）。验证：gtest 覆盖规格例子（900/100 段、18/2 重传 → 0.02）、新连接只记基线、消失连接删除、u32 回绕、本地地址不属于任何网卡时忽略；RateCounter 的基线、正常速率（比特每秒）、计数倒退取 0
  > 完成：`Retrans.*`（规格例子 0.02、新连接基线、消失删除、回环忽略、u32 回绕）和 `RateCounter.*`（基线、1 Mbps、计数倒退）。

## 2. Linux 实现（worker/netprobe）

- [x] 2.1 实现 `route`（RTM_GETROUTE 全表转储，找每块网卡的默认网关，主表优先）和网卡自动发现（getifaddrs + `/sys/class/net/*/device`）。验证：【Linux 容器】编译通过；【Linux】在 lm-worker-1 上自动选择只得到 ens160 → 172.16.255.2，并对 ens192 打一条警告；临时执行 `policy-route.sh` 后 ens192 也能找到网关 172.16.13.1（测完 `--down`）
  > 完成：lm-worker-1 上没有策略路由时只选中 ens160 → 172.16.255.2，并警告 ens192 没有默认网关；临时启用策略路由后得到 ens160 → 172.16.255.2、ens192 → 172.16.13.1（从表 101 找到），测完已撤销。
- [x] 2.2 实现 `prober`：原始 socket + `SO_BINDTODEVICE` + `ICMP_FILTER`、epoll + 100 毫秒 timerfd + eventfd 退出、按绝对时间发送并错开目标、`take_period()`。非 Linux 编为桩。验证：macOS 上桩抛异常；【Linux】见 4.2
  > 完成：macOS 上桩抛“网卡探测只支持 Linux”（`ProberStub.ThrowsOffLinux`）；Linux 上见 4.2。
- [x] 2.3 实现 `sock_diag`（NETLINK_SOCK_DIAG 取 IPv4 ESTABLISHED 连接的 cookie、本地地址、tcpi_total_retrans、tcpi_segs_out）和 sysfs 流量读取。验证：【Linux】一个小程序打印连接数和各网卡字节数，与 `ss -ti state established` 和 `/proc/net/dev` 对得上（作为 4.2 的一部分记录）
  > 完成：Linux 构建需要补 `<linux/rtnetlink.h>`（rtattr、RTA_*）。重传率与流量的实测见 4.3、4.4，没有单独写小程序。

## 3. 接入 Worker

- [x] 3.1 实现 `nic_monitor`（组合 Prober、RetransTracker、RateCounter，失败时沿用上次的值，连续 10 次升为错误），`Collector` 新增 `nics` 读取函数并填写 `Sample.nics`。验证：gtest 用假读取函数确认 nics 按顺序写入样本、为空时不写；NicMonitor 的失败沿用逻辑用注入的假数据源测试
  > 完成：`NicMonitor.AssemblesStatsAndKeepsLastOnFailure`、`CollectorNics.WritesNicsInOrderOrNone`。
- [x] 3.2 `worker/main.cpp` 支持 `nic_probe: auto|on|off`（非法值报错并包含 `nic_probe`）和 `net.*` 配置，启动日志列出网卡与目标；更新 `config/worker.yaml` 和 `install-worker.sh` 的配置模板（`net.nics` 以注释示例给出）。验证：macOS 上 `nic_probe: bogus` 报错退出；配置里写不存在的网卡时报错并包含网卡名
  > 完成：macOS 上 `nic_probe: bogus` 和不存在的网卡 eth9 都报错并以退出码 2 退出；配置模板里 `net.nics` 以注释示例给出。

## 4. 构建与 Linux 验收

- [x] 4.1 macOS 与 Linux 容器构建并跑全部测试，再跑 TSan 构建。验证：ctest 全部通过
  > 完成：macOS 构建通过；Linux 容器 88 项全部通过；TSan 构建 67 项通过（Prober 需要原始 socket，不在 TSan 中运行，见 design 第 10 节）。
- [x] 4.2 【Linux】在 lm-worker-1 上按显式配置（ens160 → 172.16.255.2、8.8.8.8；ens192 → 172.16.13.1）用 `install-worker.sh` 部署：systemd 日志列出网卡；`--dry-run` 样本 `nics` 两项且顺序正确；ens192 的 `rtt_ms` 与同期 `ping -I ens192 172.16.13.1` 平均值相差不超过 1 毫秒；同时运行 `ping` 不影响统计；以普通用户运行时 auto 打警告且 `nics` 为空，on 退出。验证：记录数值与日志
  > 完成：systemd 日志显示“网卡探测：ens160 → 172.16.255.2, 8.8.8.8；ens192 → 172.16.13.1”，共享页和 eBPF 同时启用；`--dry-run` 样本 nics 依次为 ens160、ens192；ens192 rtt 0.417 ms，同期 ping 0.260 ms，相差 0.16 ms；同时运行 ping 时超时率 0。普通用户运行时 auto 警告“ens160: 创建原始 ICMP socket 失败：Operation not permitted”，nics 为空；on 以退出码 1 退出。
- [x] 4.3 【Linux】流量与重传：经 ens192 上传 30 秒（Mac 上临时运行 `nc -l 172.16.13.1 18080 > /dev/null`，虚拟机 `nc -s 172.16.13.128`），`tx_bps` 平均值与 `/proc/net/dev` 算出的速率相差不超过 5%。验证：记录两组速率
  > 完成：经 ens192 上传 30 秒，`/proc/net/dev` 算出 6823.9 Mbps，Worker tx_bps 平均 6826.0 Mbps，相差 0.03%；没有注入时重传率 0。
- [x] 4.4 【Linux】弱网隔离：先设好 5 分钟后自动撤销的 `systemd-run`，对 ens192 加 `netem loss 10% delay 100ms` 60 秒，期间持续上传；对比注入前后：ens192 RTT 上升 90 毫秒以上、超时率 0.05–0.2、重传率 > 0.01；ens160 超时率变化 ≤ 0.03、RTT 变化 ≤ 10 毫秒。结束后确认 qdisc 已删除、Mac 上的接收端已停止。验证：记录各项平均值
  > 完成：ens192 注入前 rtt 0.43 ms、超时率 0、重传率 0；注入中 rtt 100.73 ms（+100.3）、超时率 0.057、重传率 0.113。ens160 注入前后超时率 0.049→0.065（变化 0.016），rtt 20.90→20.74 ms。另做一次对照：同一窗口 ping 丢包 8%，Worker 超时率 0.071，RTT 100.72 vs 100.55 ms。结束后 qdisc 恢复为 fq_codel，自动撤销的定时器已取消，Mac 上的接收端已停止。
- [x] 4.5 【Linux】开销：共享页、调度延迟、网卡探测全部启用时，空闲 60 秒 Worker CPU 时间 ≤ 1.2 秒。验证：记录数值
  > 完成：共享页、调度延迟、网卡探测全部启用时，空闲 60 秒 Worker CPU 时间 0.35 秒。
- [x] 4.6 更新 `README.md`（目录、里程碑表、网卡探测说明）和 `deploy/README.md`（`net.nics` 配置、CAP_NET_RAW、依赖策略路由、验收命令、回滚）。验证：文档中的配置示例能被 Worker 正确解析（用 macOS 上的配置解析单测覆盖同一段 YAML）
  > 完成：README 与 deploy/README 第 7 节已更新；文档里的配置示例与 `NetConfig.ParsesExplicitNicsInOrder` 用的 YAML 结构相同。

## Workflow follow-up

- 提交前先问用户。
