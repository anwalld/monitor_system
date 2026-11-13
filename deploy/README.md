# 部署与环境搭建（M0）

环境由 1 台 Manager 机和多台 Worker 机组成：

| 角色 | 配置 | 本目录里用到的文件 |
|------|------|------|
| Manager 机 | 4 核 8 GB，Docker + Docker Compose v2 | `docker-compose.yml`、`.env.example`、`mysql/init.sql` |
| Worker 机 | 2–4 核，2 块网卡，Ubuntu 24.04（内核 6.8） | `check-worker-host.sh`、`policy-route.sh`、`policy-route.conf.example`、`systemd/lm-policy-route.service` |

## Manager 机

### 1. 安装 Docker

```bash
curl -fsSL https://get.docker.com | sh
sudo usermod -aG docker "$USER"   # 重新登录后生效
docker compose version            # 需要 v2 及以上
```

### 2. 配置密码

```bash
cd deploy
cp .env.example .env
```

编辑 `.env`，填写 `REDIS_PASSWORD`、`MYSQL_ROOT_PASSWORD`、`MYSQL_PASSWORD` 三项：

- `.env` 已被 `.gitignore` 忽略，不要提交。
- 不要在值后面写行内注释，否则注释会被当成密码的一部分。
- 密码里有 `$` 时写成 `$$`。

三项中任何一项留空，`docker compose` 都会报出缺少的变量名并拒绝启动。

### 3. 启动

```bash
docker compose up -d --wait
docker compose ps        # redis、mysql 都应为 healthy
```

MySQL 第一次以空数据卷启动时，会自动执行 `mysql/init.sql`，建好下面六张表：

- `node`
- `node_metrics_5m`
- `node_event`
- `task`
- `ai_diagnosis`
- `ai_daily_report`

已经初始化过的数据卷不会重新执行这个文件。

### 4. 验收

```bash
set -a; . ./.env; set +a
docker compose exec redis redis-cli -a "$REDIS_PASSWORD" --no-auth-warning ping   # PONG
docker compose exec mysql mysql -u"${MYSQL_USER:-lm}" -p"$MYSQL_PASSWORD" "${MYSQL_DATABASE:-lm}" -e 'SHOW TABLES'
```

### 停止与清理

```bash
docker compose down       # 停止，保留数据
docker compose down -v    # 停止并删除数据卷（数据全部丢失）
```

## Worker 机

### 1. 安装工具并自检

```bash
sudo apt update
sudo apt install -y iproute2 linux-tools-common linux-tools-$(uname -r) tcpdump
deploy/check-worker-host.sh
```

自检只读、不修改系统，逐项输出 OK 或 FAIL，全部 OK 时退出码为 0。检查项：

| 检查项 | 要求 | 不满足时怎么办 |
|------|------|------|
| kernel | ≥ 6.8 | 换用 Ubuntu 24.04 |
| btf | 存在 `/sys/kernel/btf/vmlinux` | 换用开启 `CONFIG_DEBUG_INFO_BTF` 的内核 |
| nohz | `CONFIG_NO_HZ_COMMON=y` | Ubuntu 默认内核已开启 |
| bpftool | `bpftool btf dump` 有输出 | 安装 `linux-tools-$(uname -r)`；没有 CAP_BPF 时用 sudo 运行自检 |
| nics | 至少 2 块 UP 且有 IPv4 地址的网卡 | 在虚拟化平台给虚拟机加第二块网卡 |

### 2. 多网卡策略路由

每块网卡一张路由表：

- 源地址是这块网卡的 IP 时，从这块网卡出去。
- 绑定这块网卡发出的报文（`ping -I eth1`、`SO_BINDTODEVICE`），也从这块网卡出去。
- 其他流量仍走主路由表。

```bash
# 方式一：命令行指定。网关可省略，省略时从该网卡现有的默认路由推断
sudo deploy/policy-route.sh --nic eth0 --nic eth1:192.168.2.1

# 方式二：写配置文件（policy-route.conf 已被 .gitignore 忽略，每台机器各写各的）
cp deploy/policy-route.conf.example deploy/policy-route.conf
sudo deploy/policy-route.sh
```

编号规则：第 i 块网卡（从 0 数起）用路由表 `100+i`，from 规则优先级 `1000+i`，oif 规则优先级 `1100+i`。

- 脚本可以重复执行，不会产生重复的规则。
- 参与的网卡会把 `rp_filter` 改成 2（宽松），否则从第二块网卡回来的包会被内核丢掉。执行前的原值记在 `/run/lm-policy-route.state`。
- 撤销用 `sudo deploy/policy-route.sh --down`：删除规则，清空路由表，恢复 `rp_filter` 的原值。
- 任何一块网卡推断不到网关时，脚本报错退出，所有网卡的路由都不动。

**云平台或虚拟化平台**：要在平台侧关闭第二块网卡的“源/目的地址检查”，或者放行这块网卡的出口流量。否则主机上的路由配对了也不通。

### 3. 开机自动应用

```bash
sudo mkdir -p /opt/lm
sudo cp -r deploy /opt/lm/
sudo cp /opt/lm/deploy/systemd/lm-policy-route.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now lm-policy-route
```

这个服务依赖 `/opt/lm/deploy/policy-route.conf`，先按第 2 步写好配置文件。

### 4. 验收

假设 eth1 是第二块网卡，`<eth1 IP>` 换成它的地址：

```bash
ping -c 3 -I <eth1 IP> 8.8.8.8                          # 按源地址出网，应能通
ping -c 3 -I eth1 8.8.8.8                               # 按出接口出网，应能通
sudo tcpdump -ni eth1 icmp                              # 另开一个终端，上面两条都应在这里看到
ping -c 3 8.8.8.8                                       # 普通出网，仍走主网卡
ip rule                                                 # 每块网卡一条 from 规则和一条 oif 规则
sudo bpftool btf dump file /sys/kernel/btf/vmlinux format c | head
```
