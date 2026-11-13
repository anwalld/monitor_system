# Tasks

## 1. Worker 主机自检

- [x] 1.1 编写 `deploy/check-worker-host.sh`：检查内核 ≥ 6.8、`/sys/kernel/btf/vmlinux`、`CONFIG_NO_HZ_COMMON`（读 `/boot/config-$(uname -r)`，缺失时读 `/proc/config.gz`）、bpftool btf dump、带 IPv4 的 UP 网卡数 ≥ 2，每项一行 OK/FAIL + 实际值；验证：`shellcheck` 无告警
- [x] 1.2 【Linux】在一台 Worker 虚拟机上运行自检，全部 OK、退出码 0；临时 `ip link set <nic2> down` 后再运行，网卡项 FAIL、退出码非 0；验证运行前后 `ip rule`、`lsmod` 输出不变
  > 2026-11-13 在 lm-worker-1（172.16.255.133，Ubuntu 24.04.5 / 6.8.0-149 / aarch64）验证：五项全 OK、退出码 0（普通用户与 sudo 均通过）；`ip link set ens192 down` 后 nics FAIL、退出码 1；前后 `ip rule`、`lsmod` 不变。

## 2. 多网卡策略路由

- [x] 2.1 编写 `deploy/policy-route.sh`：参数 `--nic <name>[:<gw>]` 可多次指定，也可读 `deploy/policy-route.conf`；表号 100+序号、from 规则优先级 1000+序号、oif 规则 1100+序号；网关缺省时从 `ip route show dev <nic>` 推断，推断失败报错且不改任何网卡；验证：`shellcheck` 无告警
- [x] 2.2 在脚本中设置参与网卡 `rp_filter=2`，执行前把原值写入 `/run/lm-policy-route.state`；实现 `--down` 撤销模式，删除规则、清空路由表并恢复 rp_filter；验证：`--down` 后 `ip rule` 与执行前一致
- [x] 2.3 提供 `deploy/systemd/lm-policy-route.service`（oneshot，After=network-online.target），验证：`systemd-analyze verify` 通过
- [ ] 2.4 【Linux】在 Worker 虚拟机上验收：`ping -I <eth1 IP> 8.8.8.8` 与 `ping -I eth1 8.8.8.8` 均通且 `tcpdump -i eth1 icmp` 可见；不带 `-I` 的 ping 仍走主网卡；连续执行两次后每块网卡只有一条 from 和一条 oif 规则
  > 2026-11-13 lm-worker-1 部分验证：规则与路由表正确、执行两次无重复、ssh（ens160）不受影响；tcpdump 确认按 ens192 源地址与绑定 ens192 的 ping 从 ens192 发出，ens160 源地址与默认流量从 ens160 发出并收到应答；`--down` 后规则、路由表、rp_filter 全部恢复。**未通过项**：ens192 是 VMware host-only 网络（网关 172.16.13.1 为宿主机，无外网），经 ens192 的 ping 8.8.8.8 收不到应答。需把第二块网卡改为 NAT 或桥接后复测，暂不勾选。

## 3. Manager 基础设施

- [x] 3.1 编写 `deploy/mysql/init.sql`：按开发文档创建数据库与 `node`、`node_metrics_5m`、`node_event`、`task`、`ai_diagnosis`、`ai_daily_report` 六张表（utf8mb4）；验证：在临时 `mysql:8.0` 容器中导入无报错
- [x] 3.2 编写 `deploy/docker-compose.yml`（redis:7.2、mysql:8.0、健康检查、命名数据卷、端口 6379/3306、init.sql 挂载到 initdb 目录、必填变量用 `${VAR:?}`）和 `deploy/.env.example`；在 `.gitignore` 加入 `deploy/.env`；验证：`docker compose config` 通过、创建 `.env` 后 `git status` 不显示它
- [x] 3.3 验收 compose：`docker compose up -d` 后 60 秒内两服务 healthy；`redis-cli ping` 返回 PONG；`SHOW TABLES` 恰好六张表；未创建 `.env` 时 Compose 报缺少变量并拒绝启动；`down` 再 `up` 后数据仍在（macOS 或 Manager 虚拟机均可）

## 4. 文档与收尾

- [x] 4.1 编写 `deploy/README.md`：Manager 机（Docker 安装、`.env`、启动与验收命令）和 Worker 机（自检、策略路由、systemd 持久化、云平台关闭源/目的检查的提醒）；验证：按文档在一台新 Worker 上从头执行一遍可完成 2.4 的验收
  > 2026-11-13 在 lm-worker-1 按 deploy/README.md Worker 部分执行：自检、命令行与配置文件两种方式建策略路由、systemd 开机应用（enable 后规则生效，stop 后撤销）均按文档完成；能达到的验收结果同 2.4。测试后已禁用并删除 lm-policy-route 单元。
- [ ] 4.2 【Linux】里程碑验收：3 台 Worker 每块网卡都能单独 ping 通公网；`bpftool btf dump file /sys/kernel/btf/vmlinux format c | head` 有输出；Manager 机 Redis、MySQL 正常
  > 目前只有 1 台 Worker（lm-worker-1），单台的各项见 1.2、2.4；3 台验收待补，不勾选。

## Workflow follow-up

- 验收通过后归档本 change（`/opsx:archive`）。
