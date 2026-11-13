# Proposal

## Why

对应开发文档里程碑 M4。Worker 机都有两块网卡，并且 M0 已经按网卡配好了策略路由。但到目前为止，Worker 上报的样本里 `nics` 一直是空的，Manager 无从知道每块网卡的链路是好是坏。开发文档的评分里，网络分取“最好的那块网卡”，权重 0.2；剔除条件里也有“所有网卡超时 50%”这一条。这些都依赖每块网卡单独的 RTT、超时率、重传率和流量。M4 把这些数据采上来，填进 M1 协议里已经定义好的 `NicStat`。

## What Changes

- Worker 新增探测线程（`worker/netprobe/`）：
  - 每块网卡一个原始 ICMP socket，用 `SO_BINDTODEVICE` 绑定。所有 socket 和一个 100 毫秒的 timerfd 注册进同一个 epoll。
  - 每块网卡对配置的 1–3 个目标每 200 毫秒发一次回显请求。
  - 回包按类型、源 IP、identifier、网卡编号、序列号依次校验后计算 RTT。超过 1 秒没回的记为超时，迟到的回包丢弃。
  - 每个采集周期汇总出 RTT 均值、RTT 标准差和超时率。
- Worker 新增 TCP 重传率采集：每秒用 Netlink `NETLINK_SOCK_DIAG` 取所有 ESTABLISHED 连接的 `tcp_info`，按连接做差，按本地 IP 归到网卡，算出 ΣΔ重传 / ΣΔ发送段。
- Worker 新增流量采集：常开 `/sys/class/net/<网卡>/statistics/{rx,tx}_bytes`，每秒 `pread`，做差后换算成比特每秒。
- `Collector` 每个周期按配置顺序为每块参与探测的网卡填一条 `NicStat`。
- 新增配置：
  - `nic_probe: auto | on | off`。
  - `net.nics`：网卡名和目标列表。不写时自动选择所有有 `device` 的物理网卡，各自以默认网关为目标。
  - `net.probe_interval_ms`（默认 200）、`net.probe_timeout_ms`（默认 1000）。
- 探测不可用时（例如缺少 CAP_NET_RAW），`auto` 模式下 `nics` 留空并记警告，其他指标照常采集。

## Capabilities

### New Capabilities
- `worker-nic-quality`：Worker 选择参与探测的网卡和目标、ICMP 探测与超时判定、每周期 RTT/抖动/超时率、按网卡的 TCP 重传率和流量，以及启用与降级行为。

### Modified Capabilities
（无。M1 的 `metric-protocol` 已规定样本含 `nics`、每块网卡一项、比例字段截断到 [0, 1]，本变更按它填写，不改要求。M1 尚未归档，`openspec/specs` 为空。）

## Impact

- 对应里程碑：M4。影响模块：worker（新增 `worker/netprobe/`）、deploy（配置模板、文档）。
- 不改 proto：`NicStat` 七个字段在 M1 已定义。`rx_bps`、`tx_bps` 的单位定为比特每秒。
- Manager 不改：`node:{id}:ts` 和 `stream:metrics` 里的样本 JSON 会自动带上 `nics`。`node:{id}` Hash 暂不增加网卡字段，按网卡评分和看板展示放在 M5、M7。
- Redis 写入量：每条样本 JSON 每块网卡多约 150 字节。MySQL 不涉及。
- 运行权限：需要 CAP_NET_RAW，`lm-worker.service` 已授予，不用改。依赖 M0 的策略路由，回包才能从同一块网卡回来。
- 不在范围内：
  - 网络分、按网卡剔除（M5）。
  - IPv6 探测与 IPv6 连接的重传率。
  - 运行中网卡增减的热更新：网卡列表只在启动时确定。
  - 低频探测和急救（M6），它们走 gRPC，与 ICMP 探测无关。
