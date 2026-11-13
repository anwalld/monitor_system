# Spec Delta

## Purpose

Worker 按网卡分别测量链路质量（RTT、抖动、超时率、TCP 重传率）和流量，每秒随样本上报。这样 Manager 能看出哪块网卡变差，并按最好的网卡给节点打网络分。

## ADDED Requirements

### Requirement: 参与探测的网卡与目标
Worker SHALL 按配置 `net.nics` 确定参与探测的网卡和每块网卡的探测目标（1 到 3 个 IPv4 地址），样本中 `nics` 的顺序与配置顺序一致。

没有配置 `net.nics` 时 SHALL 自动选择：
- 网卡：`/sys/class/net/<名>/device` 存在（物理网卡）、处于 UP 状态、有 IPv4 地址。
- 目标：该网卡在任一路由表中的默认网关。找不到默认网关的网卡记一条警告，不参与探测。

以下情况 MUST 让 Worker 启动失败，错误信息中包含出错的网卡名或地址：
- 配置的网卡不存在。
- 目标不是合法的 IPv4 地址。
- 某块网卡的目标超过 3 个。

#### Scenario: 显式配置
- **WHEN** 配置 `net.nics` 为 ens160（目标 172.16.255.2、8.8.8.8）和 ens192（目标 172.16.13.1）
- **THEN** 每条样本的 `nics` 有两项，依次为 ens160、ens192

#### Scenario: 自动选择
- **WHEN** 没有配置 `net.nics`，主机有 ens160（主表默认网关 172.16.255.2）和 ens192（没有任何默认路由），还有 docker0 和回环
- **THEN** 只有 ens160 参与探测，目标为 172.16.255.2；日志中有一条警告说明 ens192 没有默认网关；docker0 和回环不参与

#### Scenario: 配置错误
- **WHEN** `net.nics` 里写了不存在的网卡 eth9
- **THEN** Worker 启动失败，错误信息包含 `eth9`

### Requirement: ICMP 回显探测
每块参与探测的网卡 SHALL 使用一个绑定到该网卡（`SO_BINDTODEVICE`）的原始 ICMP socket，按 `net.probe_interval_ms`（默认 200 毫秒）向每个目标发送回显请求：
- identifier 取进程号低 16 位，序列号按网卡递增。
- 数据部分包含网卡编号和单调时钟的发送时间。

收到的报文按以下条件依次校验，全部满足才计为一次成功，RTT = 接收时间 − 发送时间：
1. 类型为回显应答。
2. 源地址是该网卡的某个目标。
3. identifier 匹配。
4. 数据中的网卡编号与接收 socket 一致。
5. 序列号在待确认队列中。

#### Scenario: 正常回显
- **WHEN** ens192 每 200 毫秒探测 172.16.13.1，链路正常
- **THEN** 每秒约 5 次成功，样本中 ens192 的 `rtt_ms` 与同期 `ping -I ens192 172.16.13.1` 的平均值相差不超过 1 毫秒

#### Scenario: 其他进程的回包
- **WHEN** 同一主机上另一个进程（如 `ping`）也在向同一目标发回显请求
- **THEN** 那些回包因 identifier 不匹配被忽略，不影响本 Worker 的统计

### Requirement: 超时与迟到回包
每块网卡 SHALL 维护一个按发送顺序排列的待确认队列，定时器每 100 毫秒检查一次队头：等待超过 `net.probe_timeout_ms`（默认 1000 毫秒）的请求记一次超时并出队。已经记过超时的请求，之后才到的回包 MUST 丢弃，既不算成功，也不再计超时。发送失败（如网卡 DOWN）时立即记一次超时。

#### Scenario: 目标不可达
- **WHEN** 目标连续 3 秒不回应
- **THEN** 这 3 秒对应样本的 `timeout_rate` 为 1，`rtt_ms` 与 `rtt_jitter_ms` 为 0

#### Scenario: 迟到的回包
- **WHEN** 一个请求在 1.2 秒后才收到回包
- **THEN** 它只计一次超时，回包被丢弃，不计入 RTT

### Requirement: 每周期汇总
每次采集时，Worker SHALL 取出每块网卡自上次采集以来的结果，并清零：
- `rtt_ms`：成功 RTT 的平均值（毫秒）。
- `rtt_jitter_ms`：成功 RTT 的总体标准差（毫秒）。
- `timeout_rate`：超时数 / (成功数 + 超时数)，截断到 [0, 1]。

本周期既没有成功也没有超时时，三项都为 0。

#### Scenario: 部分超时
- **WHEN** 一个周期内 ens160 有 9 次成功（RTT 分别为 1、2、…、9 毫秒）和 1 次超时
- **THEN** `rtt_ms` 为 5，`rtt_jitter_ms` 约为 2.582，`timeout_rate` 为 0.1

### Requirement: 按网卡的 TCP 重传率
Worker SHALL 每个采集周期通过 Netlink `NETLINK_SOCK_DIAG` 获取所有 IPv4 ESTABLISHED 连接的 `tcp_info`，按连接标识（`idiag_cookie`）记住上次的累计重传段数和发送段数，做差后按连接的本地地址归到对应网卡：
- 网卡的 `retrans_rate` = ΣΔ重传 / ΣΔ发送段，截断到 [0, 1]。分母为 0 时为 0。
- 新连接第一次只记基线。
- 消失的连接从表中删除。
- 累计计数是 32 位的，差值按 32 位回绕计算（同一连接标识的计数只增不减）。
- 本地地址不属于任何参与探测的网卡的连接（例如回环）不计入。

#### Scenario: 计算重传率
- **WHEN** 一个周期内 ens192 上两条连接分别新增发送 900、100 段，新增重传 18、2 段
- **THEN** ens192 的 `retrans_rate` 为 0.02

#### Scenario: 新连接
- **WHEN** 一条新连接在本周期首次出现，已累计发送 5000 段、重传 50 段
- **THEN** 它本周期不计入，下个周期起按差值计入

### Requirement: 按网卡的流量
Worker SHALL 常开每块网卡的 `/sys/class/net/<名>/statistics/rx_bytes` 和 `tx_bytes`，每周期读一次。`rx_bps`、`tx_bps` = Δ字节 × 8 / Δ秒（比特每秒，Δ秒取单调时钟）。第一次读取只建立基线，取值 0；计数倒退时本周期取 0 并重新建立基线。

#### Scenario: 上传流量
- **WHEN** 经 ens192 持续上传数据 30 秒
- **THEN** 这段时间样本中 ens192 的 `tx_bps` 平均值，与同期 `/proc/net/dev` 中 ens192 发送字节数算出的速率相差不超过 5%

### Requirement: 启用与降级
Worker SHALL 支持配置项 `nic_probe`，取值为 `auto`、`on`、`off`，默认 `auto`：
- `auto`：创建原始 socket 失败（如缺少 CAP_NET_RAW）或没有任何可探测的网卡时，记一条警告，样本 `nics` 为空，其余指标照常采集。
- `on`：出现上述情况时 MUST 以非 0 退出码退出。
- `off`：不探测，`nics` 为空。

非法取值 MUST 让 Worker 启动失败，错误信息包含 `nic_probe`。运行中 Netlink 或 sysfs 读取失败时，对应字段沿用上一次的值并记警告，连续 10 次失败升为错误级别。

#### Scenario: 缺少 CAP_NET_RAW
- **WHEN** 以没有 CAP_NET_RAW 的普通用户运行 Worker，`nic_probe: auto`
- **THEN** 日志中有一条警告说明原因，样本 `nics` 为空，CPU、内存、磁盘、调度延迟照常上报

#### Scenario: 强制开启但失败
- **WHEN** 同样没有权限，`nic_probe: on`
- **THEN** Worker 记录错误并以非 0 退出码退出

### Requirement: 网卡之间互不影响
一块网卡的链路劣化 SHALL 只体现在这块网卡的指标上。

#### Scenario: 弱网注入
- **WHEN** 用 `tc qdisc add dev ens192 root netem loss 10% delay 100ms` 给 ens192 注入弱网，持续 60 秒，期间经 ens192 持续上传 TCP 数据
- **THEN** 这段时间里：
  - ens192 的 `rtt_ms` 平均值比注入前高 90 毫秒以上，`timeout_rate` 平均值在 0.05 到 0.2 之间，`retrans_rate` 平均值大于 0.01。
  - ens160 的 `timeout_rate` 平均值与注入前相差不超过 0.03，`rtt_ms` 平均值与注入前相差不超过 10 毫秒。比较的是变化量而不是绝对值，因为公网目标本身可能有固定丢包（例如经 NAT ping 8.8.8.8 约丢 11%）。

### Requirement: 采集开销
启用网卡探测（2 块网卡、共 3 个目标）后，空闲主机上 Worker 进程的 CPU 占用 SHALL 仍不超过单核的 2%。

#### Scenario: 空闲开销
- **WHEN** Worker 在空闲的 4 核虚拟机上同时启用共享页、调度延迟和网卡探测，运行 60 秒
- **THEN** 这段时间里 Worker 进程的 CPU 时间不超过 1.2 秒
