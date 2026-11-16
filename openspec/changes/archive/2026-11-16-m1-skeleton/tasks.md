# Tasks

## 1. 清理旧代码与构建骨架

- [x] 1.1 删除旧的 `worker/`、`manager/`、`proto/`（含 `manager/sql/`）及 `cmake-build-*` 引用；验证：`git ls-files worker manager proto` 为空
- [x] 1.2 重写顶层 `CMakeLists.txt`（project 名、C++17、选项 `LM_ENABLE_TSAN`/`LM_BUILD_MANAGER`/`LM_BUILD_TESTS`、FetchContent 引入 GoogleTest、`enable_testing()`）；验证：`cmake -S . -B build` 在 macOS 上配置成功
- [x] 1.3 新增 `proto/monitor.proto`、`proto/task.proto`、`proto/advisor.proto`（package lm，内容与开发文档一致）和 `proto/CMakeLists.txt` 生成静态库 `lm_proto`；验证：`cmake --build build --target lm_proto` 成功

## 2. common 库

- [x] 2.1 实现 `common/ring.h`（`lm::Ring<T, N>`，alignas(64) 槽位、四状态 CAS）；验证：gtest 覆盖空缓冲返回 false、取最新、READING 槽不被覆盖
- [x] 2.2 编写环形缓冲并发测试：生产者 1ms 写一次、消费者随机延迟读 10 秒，校验样本自洽且 seq 不倒退；验证：`-DLM_ENABLE_TSAN=ON` 下 ctest 通过且无 TSan 报告
- [x] 2.3 实现 `common/config`（yaml-cpp 读取、缺省值、类型错误时报出键名）与 `common/log`（spdlog 初始化、级别、限频辅助函数）；验证：gtest 覆盖缺省值和错误键名提示

## 3. Worker 采集

- [x] 3.1 定义 `CpuSource` 接口与 `CpuBusyCalculator`（首次只存基线、按真实差值计算、截断 [0,1]、全机取平均）；验证：gtest 用构造的读数序列覆盖首次、正常、计数回绕/差值为 0
- [x] 3.2 实现 `ProcCpuSource`（常开 `/proc/stat`、pread、只解析 cpuN 行），解析函数接受文本；验证：gtest 用 fixture 文本校验每核 idle+iowait 与 total
- [x] 3.3 实现内存采集（MemAvailable/MemTotal）与磁盘采集（`/proc/diskstats` io_ticks 差值 / 经过毫秒、排除分区/loop/ram、取最大），失败沿用上次值并告警；验证：gtest 用 fixture 覆盖设备过滤与利用率计算
- [x] 3.4 实现采集线程（绝对时间对齐的 1 秒周期，组装 Sample 写入 Ring，runq 与 nics 不设置）；验证：gtest 用假 CpuSource 运行 3 秒得到 2 条样本且 seq 递增

## 4. Worker 上报与进程

- [x] 4.1 实现上报线程（每秒 take_latest、跳过已发 seq、500ms deadline、失败不重试、日志每 10 秒限频汇总）；验证：gtest 用进程内 gRPC 假服务覆盖正常、超时、服务不可达三种情况
- [x] 4.2 实现 `worker/main.cpp`（读 `config/worker.yaml`、node_id 缺省主机名、无 /proc 时报错退出、SIGINT/SIGTERM 优雅退出）并提供 `config/worker.yaml` 样例；验证：macOS 上编译通过，运行时报“需要 /proc”并退出
- [x] 4.3 【Linux】在 Worker 虚拟机上验证采集：单核 `taskset -c 2 stress -c 1` 时 core_busy[2] ≥ 0.95；`fio` 随机读时 disk_util ≥ 0.3 且与同期 io_ticks 计算的利用率误差 ≤ 0.05；`stress --vm` 后 mem_avail_ratio 相应下降（用 `--dry-run` 模式打印样本观察）
  > 2026-11-13 lm-worker-1（4 核 aarch64）：`taskset -c 2 stress -c 1` 时 core_busy[2]=1.0、cpu_busy=0.255 ✓；`stress --vm` 占一半内存后 mem_avail_ratio 0.762→0.267 ✓。**未达标**：fio 随机读（nvme0n1，iodepth 16–64）时 disk_util 0.37–0.65，低于 0.8。对照内核 /proc/diskstats io_ticks 同期只增长 36–43%（在途 IO 32–125 个），说明计算与内核一致，是虚拟 NVMe 上 io_ticks 低估所致。用户同意把阈值改为“与内核 io_ticks 误差 ≤ 0.05”（见 design.md 第 9 节）。
  > 2026-11-13 复验：fio randread iodepth 32，每 10ms 记录 nvme0n1 的 io_ticks，与 `--dry-run` 样本逐条对比。17 拍 disk_util 0.42–0.65，最大误差 0.006 ✓。另有首拍偏差 0.51，原因是对比脚本与 Worker 同时启动、窗口起点没有记录，属测量问题。

## 5. Manager 接收与热存储

- [x] 5.1 实现 `NodeTable`（登记新节点、peer IP + worker_port 作地址、seq 检查与 3 秒重启判定、最近上报时间）；验证：gtest 覆盖新节点、乱序丢弃、重启接受、node_id 为空或超过 64 字节
- [x] 5.2 实现 Collector 服务（Report 校验与锁内更新、锁外入存储队列；OnTaskEvent 校验 kind 后返回 Ack）；验证：gtest 用进程内 gRPC 覆盖 INVALID_ARGUMENT 和 OK 路径
- [x] 5.3 实现有界存储队列与 `RedisWriter` 存储线程（pipeline 写 `node:{id}` 与 `node:{id}:ts`、每秒 XADD `stream:metrics` MAXLEN ~100000、每分钟修剪 1 小时前数据、失败限频日志）；验证：对 M0 的 Redis 运行集成测试，检查三个 key 的内容与修剪结果
- [x] 5.4 实现 `manager/main.cpp`（读 `config/manager.yaml`、Redis 密码读环境变量、每 10 秒输出在线/全部节点数、优雅退出）并提供配置样例；验证：本机启动 manager，Redis 停止时 Report 仍 50ms 内返回 OK

## 6. 部署

- [x] 6.1 在 `deploy/docker-compose.yml` 加入 manager 服务（多阶段 Dockerfile 构建、host 网络、依赖 redis healthy）；验证：`docker compose up -d manager` 后日志显示监听 50051
- [x] 6.2 编写 `deploy/systemd/lm-worker.service` 与 `deploy/install-worker.sh`（ssh 批量拷贝二进制与配置、安装并启动服务）；验证：`systemd-analyze verify` 通过，`shellcheck` 无告警
- [x] 6.3 按新结构重写 README 的目录结构、构建、运行章节；验证：按 README 在干净目录从 clone 到 ctest 全部通过

## 7. 里程碑验收

- [ ] 7.1 【Linux】3 台 Worker 用 install-worker.sh 部署并运行，Manager 日志显示 3 个在线节点；`HGETALL node:<id>` 每秒更新；`XREVRANGE stream:metrics + - COUNT 1` 含 3 个节点
  > 2026-11-13 仅 1 台验证：install-worker.sh 部署 lm-worker-1 成功；虚拟机上 compose（项目 lm）运行 redis/mysql/manager；`node:lm-worker-1` 3 秒内 seq +3，`node:lm-worker-1:ts` 与 `stream:metrics` 含该节点。3 台验收待补，不勾选。
- [x] 7.2 【Linux】故障检查：停 Manager 30 秒后恢复，Worker 采集不中断且恢复后 Manager 收到的 seq 严格递增；停 Redis 30 秒后恢复，2 秒内 `node:{id}` 恢复更新
  > 2026-11-13 lm-worker-1 验证：停 manager 30 秒期间 Worker 持续采集（恢复后 seq 跳 36），manager 启动后 4.8 秒恢复上报（含容器启动），之后 seq 严格递增（唯一一次回落是重新部署 worker 进程，286→3）；停 redis 30 秒，恢复后 0.39 秒 `node:{id}` 更新。修复：Worker 通道重连退避上限设为 1 秒，修复前 manager 重启后 26 秒才恢复。

## Workflow follow-up

- 验收通过后归档本 change（`/opsx:archive`），再开始 M2 内核模块。
