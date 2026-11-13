# 部署与环境搭建（M0、M2、M3）

环境由 1 台 Manager 机和多台 Worker 机组成：

| 角色 | 配置 | 本目录里用到的文件 |
|------|------|------|
| Manager 机 | 4 核 8 GB，Docker + Docker Compose v2 | `docker-compose.yml`、`.env.example`、`mysql/init.sql` |
| Worker 机 | 2–4 核，2 块网卡，Ubuntu 24.04（内核 6.8） | `check-worker-host.sh`、`policy-route.sh`、`policy-route.conf.example`、`systemd/lm-policy-route.service`、`udev/99-cpustat.rules`、`install-worker.sh` |

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

### 拉不到 Docker Hub 镜像时

如果 Manager 机访问 Docker Hub 超时，可以在能联网、且 CPU 架构相同的机器上准备好镜像，再通过 ssh 直接传过去：

```bash
docker pull redis:7.2 && docker pull mysql:8.0
docker build -f deploy/docker/manager.Dockerfile -t lm-manager .
docker save redis:7.2 mysql:8.0 lm-manager | gzip -1 | ssh <user>@<manager机> 'gunzip | sudo docker load'
```

传好后在 Manager 机上用 `docker compose up -d --no-build --pull never --wait` 启动。

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
sudo apt install -y iproute2 linux-tools-common linux-tools-$(uname -r) tcpdump build-essential linux-headers-$(uname -r)
deploy/check-worker-host.sh
```

自检只读、不修改系统，逐项输出 OK 或 FAIL，全部 OK 时退出码为 0。检查项：

| 检查项 | 要求 | 不满足时怎么办 |
|------|------|------|
| kernel | ≥ 6.8 | 换用 Ubuntu 24.04 |
| btf | 存在 `/sys/kernel/btf/vmlinux` | 换用开启 `CONFIG_DEBUG_INFO_BTF` 的内核 |
| nohz | `CONFIG_NO_HZ_COMMON=y` | Ubuntu 默认内核已开启 |
| bpftool | `bpftool btf dump` 有输出 | 安装 `linux-tools-$(uname -r)`；没有 CAP_BPF 时用 sudo 运行自检 |
| headers | 存在 `/lib/modules/$(uname -r)/build` | 安装 `linux-headers-$(uname -r)`（编译 cpustat 模块用） |
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

### 5. cpustat 内核模块（M2）

`install-worker.sh` 会自动完成下面这些步骤，这里列出来便于手工排查。

```bash
make -C kmod/cpustat                                   # 编出 cpustat.ko
sudo install -D -m 644 kmod/cpustat/cpustat.ko /lib/modules/$(uname -r)/extra/cpustat.ko
sudo depmod -a
echo cpustat | sudo tee /etc/modules-load.d/cpustat.conf   # 开机自动加载
sudo cp deploy/udev/99-cpustat.rules /etc/udev/rules.d/ && sudo udevadm control --reload
sudo modprobe cpustat
ls -l /dev/cpustat                                     # 应为 cr--r----- root lm
```

- 模块加载后每秒刷新一次共享页。刷新周期可用模块参数修改，例如 `modprobe cpustat interval_us=100000`，最小 1000。
- 只要有进程映射着 `/dev/cpustat`（比如 Worker 在运行），`rmmod` 就会失败。卸载前先 `systemctl stop lm-worker`。
- 内核没开 NO_HZ 时模块拒绝加载，`dmesg` 里会写明原因。
- **内核升级后**模块需要重编：重新运行 `install-worker.sh` 即可。没重编之前，Worker 会回退到 `/proc/stat`，日志里有警告。
- **Secure Boot**：开启时内核会拒绝加载未签名的模块。可以用 MOK 给模块签名，或者在 `/etc/lm/worker.yaml` 里写 `cpu_source: proc`，不用模块。
- **验收**：

  ```bash
  make -C kmod/cpustat test && sudo kmod/cpustat/test/cpustat_test   # 映射权限、周期修改、崩溃后恢复，全部 PASS
  sudo modprobe -r cpustat && sudo kmod/cpustat/test/load_loop.sh     # 映射期间卸不掉、加载卸载 1000 次无泄漏
  ./build-linux/worker/cpu_read_bench                                 # 共享页读取 P50 不超过 /proc/stat 的 15%
  ```

- **回滚**：`sudo modprobe -r cpustat && sudo rm /etc/modules-load.d/cpustat.conf`。Worker 会自动回退到 `/proc/stat`。

### 6. eBPF 调度延迟（M3）

Worker 启动时加载 `bpf/runqlat.bpf.c` 编出的程序（已嵌进 Worker 二进制），挂到 `sched_wakeup`、`sched_wakeup_new`、`sched_switch` 三个跟踪点上。

- **条件**：内核有 BTF（自检的 btf 项）；进程有 `CAP_BPF` 和 `CAP_PERFMON`（`lm-worker.service` 已授予）；装有 `libbpf1`（`install-worker.sh` 会装）。
- **配置**：`/etc/lm/worker.yaml` 里的 `runq`。
  - `auto`（默认）：加载失败只警告，样本不带调度延迟字段。
  - `on`：加载失败就退出。
  - `off`：不采集。
- **确认生效**：`journalctl -u lm-worker | grep 调度延迟` 应显示“已启用”。`redis-cli HGET node:<id> runq_p99_us` 每秒更新。
- **验收**：

  ```bash
  sudo ./build-linux/worker/runq_compare --seconds 30 > ours.json &   # 与下一条同时运行
  sudo runqlat-bpfcc 30 1 > runqlat.txt                              # bcc 自带的 runqlat
  wait; python3 bpf/test/compare_runqlat.py runqlat.txt ours.json    # 总变差距离 ≤ 0.1，P50、P99 相差 ≤ 2 倍
  ```

- **回滚**：配置里写 `runq: off` 后 `systemctl restart lm-worker`。Manager 在下一条样本时删掉 Redis 中的调度延迟字段。
