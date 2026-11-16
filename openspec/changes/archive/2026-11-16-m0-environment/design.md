# Design

## Context

见 proposal.md。当前仓库没有任何部署脚本；开发机是 macOS，Worker 是 Ubuntu 24.04 虚拟机（双网卡），Manager 机用 Docker Compose。M0 的产物都在 `deploy/` 下，后续里程碑往 compose 里追加服务、往 `deploy/` 追加脚本。

## Goals / Non-Goals

**Goals:**
- 脚本在全新的 Ubuntu 24.04 上可直接运行，只依赖系统自带工具（bash、iproute2、sysctl）和 bpftool。
- compose 文件结构为后续加入 manager、ai、nginx 预留位置（同一网络、同一 `.env`）。

**Non-Goals:**
- 不做虚拟机创建与网卡分配（由用户在虚拟化平台完成）。
- 不做批量 ssh 部署（M1 的 `install-worker.sh`）。

## Decisions

### 1. 路由表号用 100 + 网卡序号，规则优先级固定
每块网卡表号为 `100 + 序号`（eth0→100，eth1→101），from 规则优先级 `1000 + 序号`，oif 规则 `1100 + 序号`。固定编号让幂等实现简单：执行前先按优先级删除旧规则、`ip route flush table N`，再重建。
- 备选：在 `/etc/iproute2/rt_tables` 里登记表名。可读性更好，但要改系统文件、撤销也更麻烦，不采用；脚本输出里打印网卡与表号的对应关系。

### 2. 关闭严格反向路径过滤
多网卡下回包从非主网卡进来时，`rp_filter=1`（严格）会把它丢掉。脚本对参与的网卡设置 `net.ipv4.conf.<nic>.rp_filter=2`（宽松），并在撤销时恢复执行前的值（执行前记录到 `/run/lm-policy-route.state`）。
- 备选：设为 0 完全关闭。宽松模式已足够且保留基本防伪造能力。

### 3. 持久化交给 systemd，不改 netplan
M0 只提供脚本，并附一个 `lm-policy-route.service`（oneshot，`After=network-online.target`）示例，开机重新执行脚本。改 netplan 的 routing-policy 更“原生”，但各台机器网卡名、网关不同，生成 YAML 反而容易出错。

### 4. 表结构由 MySQL 镜像的 initdb 机制导入
`deploy/mysql/init.sql` 挂载到 `/docker-entrypoint-initdb.d/`，只在数据卷为空时执行一次。表结构变更在后续里程碑以新增的迁移脚本处理，不改已执行过的 init.sql。
- 备选：由 Manager 启动时建表。会让 Manager 依赖 DDL 权限，且 AI 服务也要用这些表，不采用。

### 5. 镜像版本固定
`redis:7.2`、`mysql:8.0`，带健康检查（`redis-cli ping`、`mysqladmin ping`）。必填变量用 `${VAR:?msg}` 语法，缺失时 Compose 直接报错。

## Risks / Trade-offs

- [云厂商或虚拟化平台对第二块网卡做源地址校验，策略路由正确也不通] → `deploy/README.md` 写明需要在平台侧关闭源/目的检查，自检脚本只检查主机本身。
- [网卡名在不同虚拟机上不一致（ens33、enp0s8）] → 脚本按参数或配置接收网卡列表，不写死名字。
- [macOS 开发机上无法验证路由脚本] → 路由与自检脚本的验收全部在 Worker 虚拟机上执行，tasks 中标注。

## Migration Plan

全新部署，无数据迁移。撤销：Worker 上执行 `policy-route.sh --down`；Manager 上 `docker compose down -v` 删除容器与数据卷。
