# Proposal

## Why

开发文档里程碑 M0：后续所有里程碑都要跑在 1 台 Manager + 多台双网卡 Worker 的环境上，内核模块、eBPF、多网卡探测分别依赖 BTF、NO_HZ 和按网卡分流的策略路由。先把环境做成可重复执行的脚本和配置，后面的里程碑才能在一致的环境上验收。

## What Changes

- 新增 `deploy/check-worker-host.sh`：检查 Worker 主机是否满足内核 6.8、BTF、NO_HZ、至少 2 块网卡、bpftool 可用，逐项输出结果，不满足时非零退出。
- 新增 `deploy/policy-route.sh`：为每块网卡建一张独立路由表，按源地址和出接口两条规则分流，可重复执行（幂等）、可撤销。
- 新增 `deploy/docker-compose.yml`（M0 只起 Redis、MySQL）和 `deploy/.env.example`；密码只放 `.env`，`.env` 加入 `.gitignore`。
- 新增 `deploy/mysql/init.sql`：按开发文档建 `node`、`node_metrics_5m`、`node_event`、`task`、`ai_diagnosis`、`ai_daily_report` 六张表，MySQL 容器首次启动时自动导入。
- 新增 `deploy/README.md`：Manager 机与 Worker 机的环境搭建步骤与验收命令。

## Capabilities

### New Capabilities
- `worker-host-preflight`: Worker 主机环境自检，判定主机能否运行内核模块、eBPF 和多网卡探测。
- `worker-policy-routing`: Worker 多网卡策略路由，保证从某块网卡的地址或绑定该网卡发出的流量从这块网卡出去。
- `manager-infra-stack`: Manager 侧基础设施编排，一条命令起 Redis 和 MySQL，并初始化表结构，敏感配置不进 Git。

### Modified Capabilities
（无）

## Impact

- 对应里程碑：M0。影响模块：deploy。
- 新增目录 `deploy/`，不改动现有 C++ 代码；旧的 `manager/sql/init_server_performance.sql` 被 `deploy/mysql/init.sql` 取代，旧文件随 M1 清理旧代码时删除。
- 依赖：Manager 机需要 Docker 与 Docker Compose v2；Worker 机需要 iproute2、bpftool。
- 不在范围内：manager、ai、nginx 容器（M1、M7、M8 再加入 compose）；Worker 安装脚本 `install-worker.sh`（M1 有 worker 二进制后再写）；按业务组切换出口网卡（开发文档非目标）。
