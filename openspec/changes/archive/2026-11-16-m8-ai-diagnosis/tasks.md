# Tasks

## 1. AI 服务

- [x] 1.1 `windows` 与 `detector`：滑动窗口、稳健 z 分数（MAD 下限、单向）、连续 3 点、阈值规则（10 秒平均）、事件规则、严重度、去重。验证：pytest 覆盖持续越界报一次、单点毛刺不报、常量序列、样本不足、方向、去重与升级
  - 结果：持续越界在第 3 个点报一次；单点、两点毛刺不报；超时率常量 0 时 0.1 不报、0.5 报；11% 基线丢包不触发 20% 阈值
- [x] 1.2 `schema`、`context`、`llm`：pydantic 校验、上下文组装（10 秒降采样、对照中位数）、匿名化往返、厂商选择、重试与超时、token 预算、统计诊断。验证：pytest 用假客户端覆盖合格、不合格重试一次、超时重试 2 次后放弃、预算用尽、未选厂商
  - 结果：`ai/tests` 共 24 个用例通过（检测 14、LLM 与匿名化 10）
- [x] 1.3 `store`、`diagnosis`、`api`、`main`：consumer group、预热、积压跳过、MySQL 写入与补写、XADD 诊断事件、REST 接口、鉴权。验证：本机测试 Redis + MySQL + `mock_llm.py`，回放样本触发诊断，接口返回正确
  - 结果：Mac 上 `lmtest-redis`、`lmtest-mysql`、`mock_llm.py`，`replay_metrics.py` 合成 3 节点：预热 915 条样本后，CPU、可用内存、调度延迟（被去重）、剔除事件都在第 3 个越界点检出；诊断写入 MySQL 并发出 diagnosis 事件。mock 不响应：3 秒超时 ×3（退避 1、2 秒）后写统计诊断；第一次不合格：修正后合格（attempts 2）；两次不合格：统计诊断（invalid）；暂停 MySQL：事件先发，恢复后补写
- [x] 1.4 `config/ai.yaml`、`ai/requirements.txt`、`deploy/docker/ai.Dockerfile`。验证：镜像构建，容器内 pytest 全过
  - 结果：`lm-ai` 镜像内 24/24 通过

## 2. 看板与部署

- [x] 2.1 看板：诊断页、设置页、事件页 `diagnosis` 摘要、节点详情相关诊断、AI 不可用提示。验证：`npm run build`；浏览器里各页正常
  - 结果：构建 607 KB（gzip 210 KB）；内置浏览器里诊断页（筛选、展开详情、上下文）、设置页（切换厂商后显示“缺少 API Key”）正常
- [x] 2.2 nginx `/api/ai/`、compose `ai` 服务、`.env.example`。验证：本机经 Nginx 访问诊断页和设置页
  - 结果：虚拟机 `lmtest-nginx`（8088）经 `/api/ai/` 访问 AI、经 `/api/` 访问 Manager 都正常

## 3. 验收

- [x] 3.1 【Linux】部署 AI 服务和新看板到虚拟机（LLM 先用 Mac 上的 `mock_llm.py`）
  - 结果：测试容器 `lmtest-ai`、`lmtest-mockllm`（虚拟机本机 18100）、`lmtest-nginx`（8088），连正式 Redis/MySQL；compose 项目未改动。预热 599 条样本。之后经用户同意并入 compose 项目 `lm`（`ai` + `lm-web:m8`，80 端口），测试容器删除
- [x] 3.2 【Linux】故障注入检出：CPU（stress -c 4）、调度延迟（8 线程挤一个核）、内存（stress --vm）、磁盘（fio）、网卡（ens192 netem 延迟 + 丢包）、进程停止（剔除）各至少 2 次；统计检出率、检出延迟（`ai_eval.py`）
  - 环境变化：15:03 起虚拟机上有用户的 ROS 2 Jazzy + Gazebo 仿真（`~/robot_ws`）反复启动，CPU 到 0.999、调度延迟 2.5 ms、可用内存 0.85 → 0.48，Manager 剔除 3 次；AI 全部检出（这些不在注入记录里，统计时算作“误报”）。CPU、内存、磁盘、网卡、停止五类改到 Mac 上的干净测试节点 `lmtest-worker2`（`lmtest-w2img`：lm-builder + iproute2/fio，两块网卡 eth0/eth1，eth1 上 netem，停止用 `docker pause`），并按用户要求停掉同一 Docker 虚拟机里的 cppaiserver 容器
  - 测试节点（5 类 × 2 轮）：检出 10/10；延迟中位数 cpu 4.5 s、mem 4.3 s、disk 4.7 s、nic 3.9 s（先 RTT 后超时率）、stop 2.9 s（evict）；CPU 故障同时触发 evict
  - 虚拟机（ROS 之前的第 2 轮，第 1 轮记录随换容器丢失）：runq 4.2 s、mem 4.6 s、disk 4.0 s、nic（ens192）3.8 s、stop 2.5 s
  - 修改上下文后复测：测试节点 stop 3.0 s / 2.7 s、nic 4.5 s、cpu 4.0 s；虚拟机 runq 4.9 s、46.8 s（ROS 让 CPU 长期 0.9 以上、P99 中位数升到约 2 ms，越界更晚）。合计检出 16/16 次注入
- [x] 3.3 【Linux】空闲至少 1 小时统计每小时误报次数
  - 17:01–18:01，不做任何注入：干净测试节点 `lmtest-worker2` 0 次；虚拟机 lm-worker-1 32 次（调度延迟 24、可用内存 3、磁盘 2、ens160 RTT 2、剔除 1），全部发生在用户的 ROS 仿真反复启停期间，是真实的本地负载变化而不是检测器噪声；同一节点 10 分钟去重后实际诊断约 6 条/小时
- [x] 3.4 【Linux】故障隔离：压测期间停 AI 服务、LLM 一直超时、MySQL 不可用，调度无失败、看板其他页正常，AI 恢复后继续
  - 停 AI 服务 60 秒（lm-bench 10 个/秒 × 120 秒，两节点）：1158 个任务全部完成，P99 215 ms；期间经 nginx `/api/nodes` 200、`/api/ai/health` 502；启动后预热即恢复检测
  - LLM 一直不响应（mock hang）：30 秒超时 × 3（退避 1、2 秒）后写统计诊断，检测不中断
  - MySQL 阻断 60 秒（iptables 丢回环 3306，systemd-run 自动撤销）：1440 个任务全部完成，P99 213 ms；两条诊断事件照发（diag_id 为 null），恢复后 4 秒补写
  - 注：第一次压测全部 UNAUTHENTICATED，是取 worker.yaml 里的 token 时带上了引号，去掉后重测
- [x] 3.5 【Linux】真实 LLM（需要用户提供厂商、模型和 Key）：每类故障至少 1 次诊断，JSON 合格率
  - 用户选 DeepSeek（deepseek-chat），在设置页填 Key（BYOK，见 design）。第一次填的 Key 被拒（401），诊断降级为统计结果；换 Key 后 11 次调用全部合格（1 次修正重试后合格），每次约 5000 token
  - 首要原因（人工判断）：测试节点 CPU ✓（本地 CPU 密集进程，0.9）、磁盘 ✓（I/O 密集任务，0.85）、停止 ✗（归因成 eth1 网络故障：前一次网卡故障的高 RTT 还在上下文里，且上下文没说上报中断）；虚拟机 ROS 负载期间的 8 条都归因为 CPU 争抢导致调度延迟升高 ✓
  - 修改后：停止 ×2 ✓（上报中断，缺失 162 秒样本）、网卡 ✓、CPU ✓、调度延迟 ✓、内存 ✓（#35，0.84 → 0.54，z=15.08，内存被突发占用）。六类故障都有合格的 DeepSeek 诊断
  - 据此修改：上下文加 `reporting`（最后样本距今秒数、10 分钟缺失秒数），提示词说明 lost/probe 与“网卡变差”的区别；相关异常按指标去重（磁盘反复越界曾重复 8 次）；容器挂 `/etc/localtime`，模型引用本地时间
- [x] 3.6 更新 README、deploy/README、HANDOFF
