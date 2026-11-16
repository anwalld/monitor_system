# Design

## Context

见 proposal.md 的 Why。现状和约束：

- **协议与采集**：`lm::NicStat` 已在 `proto/monitor.proto` 定义（name、rtt_ms、rtt_jitter_ms、timeout_rate、retrans_rate、rx_bps、tx_bps）。`Collector` 通过 `MetricReaders` 注入各项读取函数，M3 新增了 `runq`。
- **Worker 权限**：systemd 单元授予 `CAP_NET_RAW`，可以创建原始 socket 并使用 `SO_BINDTODEVICE`（5.7 起需要 CAP_NET_RAW）。
- **配置读取**：`lm::Config` 只支持按点路径取标量，读 `net.nics` 这种列表需要扩展。
- **虚拟机 lm-worker-1**：
  - ens160：NAT，默认网关 172.16.255.2，能 ping 通 8.8.8.8（约 48 ms）。
  - ens192：host-only，只能到宿主机 172.16.13.1。
  - 策略路由目前已撤销，ens192 在主表里只有直连路由，没有默认路由。
  - 两块都是有 `device` 的物理网卡。
  - 有 tc，没有 iperf3。
- **开发机**：macOS 没有原始 ICMP 的 `SO_BINDTODEVICE`、epoll、Netlink，探测必须能在 macOS 上编译并对纯逻辑做单测。

## Goals / Non-Goals

**Goals:**
- 探测线程按开发文档的单线程 epoll + timerfd 结构实现。
- 报文构造与解析、待确认队列与超时、统计汇总、重传差分、速率计算都写成与平台无关的纯逻辑，在 macOS 上单测。
- 一块网卡出问题不影响另一块，也不影响其他指标的采集。

**Non-Goals:**
- 不做 IPv6、不做 TCP/UDP 探测、不测带宽上限。
- 不按目标分别上报，同一块网卡的多个目标合并统计。

## Decisions

### 1. 目录与模块
```
worker/netprobe/icmp.{h,cpp}          回显请求构造、校验和、应答解析（含 IPv4 头）
worker/netprobe/probe_state.{h,cpp}   每网卡的待确认队列、超时、周期累计（纯逻辑，时钟由调用方传入）
worker/netprobe/prober.{h,cpp}        探测线程：原始 socket、epoll、timerfd（仅 Linux，非 Linux 编为桩）
worker/netprobe/retrans.{h,cpp}       RetransTracker：按 cookie 差分并按本地地址归网卡（纯逻辑）
worker/netprobe/sock_diag.{h,cpp}     Netlink NETLINK_SOCK_DIAG 取 tcp_info（仅 Linux）
worker/netprobe/traffic.{h,cpp}       RateCounter（纯逻辑）+ sysfs 读取
worker/netprobe/nic_config.{h,cpp}    解析与校验 net.nics；自动发现网卡和默认网关
worker/netprobe/route.{h,cpp}         Netlink RTM_GETROUTE 全表转储，找每块网卡的默认网关（仅 Linux）
worker/netprobe/nic_monitor.{h,cpp}   汇总以上，每周期产出 std::vector<lm::NicStat>
```
CMake 新增静态库 `lm_worker_netprobe`，`worker` 和测试链接它。Linux 专用的实现用 `#if defined(__linux__)` 包住，其余平台编译为抛异常的桩。

### 2. 线程与数据流
- **探测线程**（新增，`Prober` 持有）：独占所有原始 socket、timerfd 和 epoll，负责发送、接收、超时判定，把结果累计进每网卡的 `ProbeState`。
- **采集线程**（已有）：每周期调用 `NicMonitor::collect()`，依次做三件事：
  1. 调 `Prober::take_period()` 取出并清零每网卡的周期累计。这一步持 `Prober` 内部的一把互斥锁，只拷贝几个计数，耗时在微秒级。
  2. 发一次 sock_diag 请求，算重传率。
  3. 读 sysfs，算流量。
- 上报线程不变，`NicStat` 随样本进入环形缓冲。
- **退出**：`Prober::stop()` 写一个 eventfd 唤醒 epoll，线程退出后关闭所有 fd。

### 3. 探测细节
- **socket**：`socket(AF_INET, SOCK_RAW, IPPROTO_ICMP)`，然后：
  - `setsockopt(SO_BINDTODEVICE, 网卡名)`。
  - `ICMP_FILTER` 只放行回显应答，减少无关唤醒。
  - 非阻塞，加 `SOCK_CLOEXEC`。
- **发送调度**：
  - timerfd 每 100 毫秒触发一次。
  - 每个目标记一个下次发送的绝对时间，到期就发送，再加上一个周期（落后超过一个周期时对齐到当前时间，避免补发）。
  - 同一块网卡的多个目标在启动时错开 `interval / 目标数`，避免同时发出。
- **报文**：
  - ICMP 头：type 8，identifier = `getpid() & 0xffff`，seq 按网卡递增（u16 回绕）。
  - 数据 16 字节：网卡编号（u32）、保留（u32）、发送时间（u64，CLOCK_MONOTONIC 纳秒）。
  - 校验和按 RFC 1071 计算。
- **接收**：原始 IPv4 ICMP socket 收到的数据带 IP 头。按 IHL 跳过 IP 头后，依规格的 5 个条件校验。RTT 用 epoll 返回后读到报文时的单调时钟减去待确认队列里记的发送时间。不使用报文里的时间戳，以免被伪造的回包影响。
- **待确认队列**：`std::deque<Pending{seq, target_index, sent_ns}>`。
  - 回包按 seq 在队列里线性查找。每块网卡最多约 3 目标 × 5 次/秒 × 1 秒 = 15 项，线性查找足够。
  - 找到就出队并计成功；中间更早的请求仍留在队列里，等各自超时。
  - 超时检查只看队头。
- **发送失败**（`sendto` 返回 ENETDOWN、ENETUNREACH、EHOSTUNREACH 等）：直接计一次超时，不入队。

### 4. 周期汇总（ProbeState）
- 累计成功次数 n、RTT 之和、RTT 平方和、超时次数。
- `take()` 返回 `{rtt_ms = 均值, jitter_ms = sqrt(max(0, E[x²] − E[x]²)), timeout_rate}` 并清零。
- 用平方和公式会有精度问题，但 RTT 在毫秒量级、每周期十几个样本，用 double 足够。

### 5. TCP 重传率
- **请求**：
  - `socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC, NETLINK_SOCK_DIAG)`。
  - 发送 `inet_diag_req_v2{AF_INET, IPPROTO_TCP, idiag_ext = 1 << (INET_DIAG_INFO − 1), idiag_states = 1 << TCP_ESTABLISHED}`，带 `NLM_F_DUMP`。
  - 逐条解析 `inet_diag_msg` 和 `INET_DIAG_INFO` 属性。属性长度不足以覆盖 `tcpi_segs_out` 时跳过该连接。
- **差分**：`RetransTracker` 维护 `unordered_map<uint64 cookie, {u32 retrans, u32 segs}>`，每次更新时：
  - 新 cookie 只记基线。
  - 已有的按 u32 模运算求差。
  - 本次没出现的 cookie 删除。
  - 差值按 `local_ip → 网卡编号` 映射累加，映射在启动时由 `getifaddrs` 建立。
- **权限**：读取所有连接的 `tcp_info` 不需要特权（`ss -ti` 普通用户也能看）。
- **失败处理**：请求失败时，各网卡的 `retrans_rate` 沿用上次的值并记警告，连续 10 次升为错误。基线保留，恢复后接着算。

### 6. 流量
- 每块网卡常开两个 fd（`rx_bytes`、`tx_bytes`），每周期 `pread` 并解析十进制数。
- `RateCounter::update(bytes, now_ns)`：第一次返回 0；计数倒退时返回 0 并重建基线；其余情况返回 Δ×8×1e9/Δns。
- 读取失败时沿用上次的值。

### 7. 网卡与目标的选择
- **配置格式**：

  ```yaml
  nic_probe: auto            # auto | on | off
  net:
    probe_interval_ms: 200
    probe_timeout_ms: 1000
    nics:                    # 不写则自动选择
      - name: ens160
        targets: [172.16.255.2, 8.8.8.8]
      - name: ens192
        targets: [172.16.13.1]
  ```

- `lm::Config` 增加 `YAML::Node node(const std::string& key) const`，供解析列表使用，其余接口不变。
- **校验**：
  - 网卡名必须在 `/sys/class/net/` 下。
  - 目标用 `inet_pton` 校验，数量 1–3。
  - `probe_interval_ms` 不小于 50，`probe_timeout_ms` 不小于 interval。
  - 出错抛 `ConfigError`，消息里带网卡名或地址。
- **自动选择**：
  - 遍历 `getifaddrs`，取有 IPv4 地址、`IFF_UP`、并且 `/sys/class/net/<名>/device` 存在的网卡。
  - 再用 RTM_GETROUTE 转储所有路由表（`rtm_table` 和 `RTA_TABLE` 都看），找 `rtm_dst_len == 0` 且 `RTA_OIF` 为该网卡的路由，取它的 `RTA_GATEWAY`。主表优先，其次是编号最小的表。所以 M0 策略路由生效时，第二块网卡也能找到网关（在表 101 里）。
  - 找不到默认网关的网卡记警告并跳过。

### 8. 启用与降级
- `nic_probe` 的校验与 `cpu_source`、`runq` 放在一起，都在检查 /proc 之前。
- 构造 `NicMonitor` 时，以下情况抛 `std::runtime_error`，由 main 按 auto、on 分别处理：
  - 解析配置出错：抛 `ConfigError`，任何模式下都让启动失败。
  - 没有可探测的网卡。
  - 创建任一原始 socket 失败：附带 errno，例如“ens160: Operation not permitted”。
- `auto` 失败时 `readers.nics` 留空，`nics` 为空。
- 启动日志列出参与探测的网卡和目标，例如“网卡探测：ens160 → 172.16.255.2, 8.8.8.8；ens192 → 172.16.13.1”。

### 9. 接入 Collector
- `MetricReaders` 新增 `std::function<std::vector<lm::NicStat>()> nics`。为空，或返回空向量时，样本 `nics` 为空。
- 第一次采集时 CPU 只建基线，不出样本，`NicMonitor` 的流量和重传率也同时完成基线，所以第一条样本里这两项就有有效值。

### 10. 实现中补充的决定（2026-11-13）
- **网卡之间互不影响按变化量验收**：经 NAT ping 8.8.8.8 本身就有约 11% 的固定丢包，ens160 注入前的超时率已经是 0.05 左右。所以规格里 ens160 的判据改为“与注入前相差不超过 0.03”。比较变化量而不是绝对值，才是“互不影响”的本意。
- **proto JSON 里的 uint64 是字符串**：`rx_bps`、`tx_bps` 在 `MessageToJsonString` 的输出中带引号，这是 proto3 JSON 的标准行为。下游解析时要转成数字。
- **TSan**：`ProbeState` 本身不是线程安全的，由 `Prober` 内部的互斥锁保护。`Prober` 需要原始 socket，只在虚拟机上验收，所以 TSan 构建覆盖的是其余纯逻辑和 Collector。
- **验收时的超时率**：netem 丢 10% 时，Worker 统计的超时率在 5.7%–7.1% 之间。同一窗口里 `ping` 丢包 8%，两者一致，差距来自每 30 秒只有约 150 个包的抽样波动。

## 性能与资源影响

- **探测线程**：每秒约 10 次 timerfd 唤醒，加上每个目标 5 次发送和 5 次接收。2 块网卡、3 个目标时每秒约 40 次系统调用，CPU 可以忽略。
- **网络**：每个目标每秒 5 个 44 字节的回显请求，约 1.8 kbps。
- **采集线程**：每秒多一次 Netlink dump（与连接数成正比，Worker 机连接很少），加 2×网卡数 次 pread。
- **锁**：`Prober` 内部一把互斥锁，探测线程每次记账、采集线程每秒取一次时各持有，都只有几次加法和拷贝。
- **Redis**：样本 JSON 每块网卡约多 150 字节，写入次数不变。Manager 锁不受影响。

## Risks / Trade-offs

- [没有策略路由时，绑定第二块网卡发往非直连目标的报文无路可走] → 这正是该网卡“不通”的真实反映，表现为超时率 1。部署文档强调先跑 `policy-route.sh`。验收用的 ens192 目标是直连的宿主机，不依赖策略路由。
- [宿主机或网关限速 ICMP] → 每目标每秒 5 次，远低于常见的限速阈值。验收时对照 `ping -I` 的结果确认。
- [RTT 包含 Worker 线程的调度延迟] → 探测线程只做 IO，唤醒延迟在微秒级，相对于毫秒级 RTT 可以忽略。规格要求与 ping 相差不超过 1 毫秒。
- [netem 验收需要经 ens192 的 TCP 上传流量，而 ens192 只能到 Mac] → 在 Mac 上临时运行一个丢弃数据的 TCP 接收端（例如监听 172.16.13.1:18080 的 `nc -l`），虚拟机绑定 ens192 的地址上传，测完就停掉。上传方是虚拟机，重传也在虚拟机侧统计。
- [netem 规则残留，影响后续测试] → 注入前用 `systemd-run --on-active=5min tc qdisc del dev ens192 root` 设好自动撤销，与 M0 测策略路由时的做法一致。netem 只加在 ens192 上，ssh 走 ens160，不受影响。

## Migration Plan

1. 用 `install-worker.sh` 升级 Worker。已有的 `/etc/lm/worker.yaml` 会保留，没有 `net.nics` 时走自动选择。需要指定目标时，手工补上 `net.nics`。
2. Manager 不需要升级。
3. 回滚：配置里写 `nic_probe: off`，或者换回 M3 的二进制。
