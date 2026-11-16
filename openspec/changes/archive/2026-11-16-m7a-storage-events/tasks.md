# Tasks

## 1. 纯逻辑

- [x] 1.1 `MetricsAggregator`：MetricPoint、5 分钟对齐、累计、取出已结束的桶、全部取出。验证：gtest 覆盖 spec 聚合口径例子、桶边界对齐、缺失列为空、5 秒宽限
  - 结果：`tests/test_cold_storage.cpp` 4 个用例通过（含 spec 例子 cpu_avg 0.4 / max 0.6 / P99 300 / mem 0.3）
- [x] 1.2 `TaskMerger`：合并规则（时间取第一次、状态等级、timeout 被 done/failed 覆盖）、有无分配信息分流、上限。验证：gtest 覆盖正常任务、迟到完成、作废、乱序到达（received 先于 sent）
  - 结果：4 个用例通过；上限超出时丢弃新任务并计数
- [x] 1.3 事件生成：StateChange → NodeEvent 的 kind 与 detail，急救结束 detail。验证：gtest 覆盖表中每种触发
  - 结果：`Events.FromStateChange`、`Events.RescueEnd` 通过，detail 逐字节比对

## 2. Manager 接线

- [x] 2.1 `NodeTable`：概率写入 NodeState 并随样本带出；StateChange 增加 detail_json（急救开始）；`RescueSession` 结束统计回调。`SchedulerService`、`CollectorService`、`SchedulerLoop` 生成 TaskChange。验证：gtest 覆盖两台节点概率 0.6/0.4、迟到事件也生成 done 变化
  - 结果：`NodeTableProb.TwoNodesSixtyForty`（分数 0.9 / 0.6 → 概率 0.6 / 0.4，失联后 0）、`RescueStartDetail`、`TaskProducers.*`（迟到 done/failed、Allocate/MarkSent、作废/未确认/执行超时）通过
- [x] 2.2 `RedisWriter`：事件 XADD 并取回 id；样本摘要、新节点、事件转给 MySQL 队列；`prob` 字段。验证：Redis 集成测试检查 stream:events 字段与 MAXLEN
  - 结果：`RedisWriterTest.EventsStreamAndForwardToMysql` 通过：字段 node/kind/ts_ms/detail、`prob` 0.6000、转给 MySQL 的事件 id 与 entry id 一致且顺序不变（rescue_end 在后）、MAXLEN ~5 写 200 条后长度 < 200
- [x] 2.3 `MysqlClient`（C API 封装 + 桩）、`MysqlWriter`（线程、四张表的 SQL、每秒刷新、退出时全部写出、失败限频日志、重连）；CMake 探测库；`main.cpp` 与 `mysql` 配置。验证：MySQL 集成测试（`LM_TEST_MYSQL_*` 环境变量，未设置则跳过）在临时 MySQL 容器上检查四张表
  - 结果：`lmtest-mysql`（mysql:8.0 + init.sql）上 `MysqlWriterTest.FourTables`、`ThreadWritesAndUnreachableDoesNotBlock` 通过（macOS 与 Linux 容器都跑过）：node 一行；第一个桶 300 样本 cpu_avg 0.3，第二个桶在宽限内不写、退出时写出、重复写覆盖为 samples 2；event_id 落在 detail；task a=done、v=void（迟到 done 不覆盖）、x=timeout→done；连不上时 stop < 3 秒
  - 发现并修复：mariadb-connector-c 的 `mysql_escape_string` 无连接时段错误，改为未连接时内置转义

## 3. 部署与验收

- [x] 3.1 macOS、Linux 容器构建并全部测试通过；Manager 镜像加 MySQL 运行库；compose 传 `LM_MYSQL_PASSWORD`。验证：ctest 全部通过，镜像能启动并连上 MySQL
  - 结果：Linux 容器 222/222 通过（含 Redis、MySQL 集成测试，连 Mac 上的 lmtest 容器）；Manager 镜像（运行库 libmariadb3、libdrogon1t64）在虚拟机启动即连上 MySQL，日志每 10 秒汇总 MySQL 写入行数
- [x] 3.2 【Linux】部署后跑 11 分钟：`node_metrics_5m` 每节点每 5 分钟一行、samples 约 300；压测一段任务后 `task` 表状态正确（done、void、timeout→done）；剔除一次节点后 `stream:events` 与 `node_event` 有对应事件且 id 一致；`node:{id}` 有 `prob`。验证：记录查询结果
  - 环境：lm-worker-1 + Mac 上的 lmtest-worker2，2026-11-14 05:42 UTC 部署，运行约 23.5 小时（中间虚拟机关机 8 小时）
  - `node_metrics_5m`：每节点 282 行，桶间隔 5 分钟；lm-worker-1 连续桶 samples 300（平均 298.8），lmtest-worker2 平均 253.5（Mac 休眠、CPU 忙时被剔除）。例：lm-worker-1 05:45 桶 cpu_avg 0.022、cpu_max 0.053、P99 最大 888 µs、mem 最小 0.748、RTT 19.05 ms、score 0.973、samples 300；lmtest-worker2 runq 与网络列为 NULL（未采集）
  - 虚拟机 12:49 关机时 Manager 正常退出写出未结束的桶（12:50 桶 70 样本）；重启后自动续写。20:50 桶出现 341 样本，是开机时系统时钟回拨导致同一时间段重复计入（按墙上时间分桶）
  - `task`：压测 322 个 + 作废 5 个 + 310 秒长任务 5 个 → done 325、void 5、failed 2（Worker 并发满）；5 个长任务 300 秒记执行超时（日志），310 秒迟到完成后为 done，t_done = t_recv + 310 s
  - 事件：暂停 lmtest-worker2 → 3.0 秒后 `evict`（lost），恢复后 `slow_start`（from evicted）；`node_event.detail.event_id` 与 `stream:events` entry id 逐条一致；`node:lm-worker-1` 的 `prob` 0.7851（两台慢启动时 0.647 / 0.353）
- [x] 3.3 【Linux】MySQL 停 60 秒：Allocate 与 Redis 照常，恢复后继续写入。验证：记录日志与查询结果
  - 方法：不停用户的 MySQL 容器，iptables 丢回环口 3306 共 70 秒（systemd-run 自动撤销）
  - 第一次：断开期间 169 个任务全部完成（P99 212 ms），Redis 正常，日志每 10 秒一条“MySQL 不可用（缓冲 任务 N、未写的桶 2）”；恢复后写入、05:15 桶补写。但断开后第一批（发现 Lost connection 的那一次）5 行任务丢失 → 修复为连接断开时放回缓冲（见 design），加集成测试 `LostConnectionKeepsBatchForRetry`（KILL 写线程连接）
  - 修复后重测：断开期间 228 个任务全部完成（P99 214 ms）+ 3 个作废，缓冲涨到 231；恢复后一次写入，task 表 done 228 + void 3，一行不少
- [x] 3.4 更新 README 与 deploy/README（事件流、MySQL 表、配置、验收命令）

## Workflow follow-up

- m7a 与 m7b 都完成后一起提交（M7 一个提交）。
