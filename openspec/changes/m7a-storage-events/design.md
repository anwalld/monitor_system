# Design

## Context

- 存储线程（`RedisWriter`）从 `StorageQueue<AcceptedSample>` 取上报和“只含状态”的记录，用 pipeline 写 Redis。
- 状态变化由 `StateSink` 在锁外处理（打日志、推只含状态的记录）；急救结束在 `RescueSession` 里打日志。
- 在途账的变化发生在 `SchedulerService::Allocate/MarkSent`、`CollectorService::OnTaskEvent`、`SchedulerLoop`（超时）里，都已经在锁外拿到结果。
- MySQL 表结构在 `deploy/mysql/init.sql`（M0）。

## Goals / Non-Goals

**Goals**
- 事件同时进 `stream:events` 和 `node_event`，看板和 AI 都只读这一个流。
- MySQL 的写入与调度、Redis 完全解耦，MySQL 挂掉不影响调度和热数据。
- 聚合、任务合并都是可单测的纯逻辑。

**Non-Goals**
- 读接口（m7b）、AI 表（M8/M9）。

## Decisions

### 1. 模块

```
manager/storage/
  events.{h,cpp}          NodeEvent、事件生成（StateChange → NodeEvent，急救结束的 detail）
  metrics_agg.{h,cpp}     MetricsAggregator：按节点、按 5 分钟桶累计，取出已结束的桶
  task_rows.{h,cpp}       TaskChange、TaskMerger：1 秒内合并同一任务的多次变化
  mysql_client.{h,cpp}    MySQL C API 的薄封装（连接、重连、执行、转义）；没有库时为桩
  mysql_writer.{h,cpp}    MysqlWriter：独立线程，消费 MysqlQueue，写四张表
  redis_writer.*          增加 XADD stream:events，并把样本摘要、事件转给 MySQL 队列
```

### 2. 数据流与线程

```
gRPC 线程 / 调度主循环 / 急救线程
   ├─ StateSink(StateChange) ─► 事件 NodeEvent ─► StorageQueue（AcceptedSample.event）
   └─ TaskChange（分配/MarkSent/事件/超时）────────────────► MysqlQueue
存储线程（Redis）：样本 → node:{id}、ts、stream:metrics；事件 → XADD stream:events
                   └─ 样本摘要 MetricPoint、新节点 NodeRow、事件 ──────────► MysqlQueue
MySQL 线程：MysqlQueue → 聚合器 / 任务合并 / 事件缓冲 → 每秒批量写
```

- 事件先经过 Redis 存储线程再进 MySQL 队列：`XADD` 返回的 entry id 就是事件 id，MySQL 的 `node_event.detail` 里也存这个 id，两边能对上。Redis 不可用时事件 id 为空，MySQL 照样写。
- `MysqlQueue` 是 `BoundedQueue<MysqlItem>`（容量 16384，满时丢最旧并计数），`MysqlItem` 是 `std::variant<MetricPoint, NodeRow, NodeEvent, TaskChange>`。

### 3. 事件

| 触发 | kind | detail |
|---|---|---|
| 新节点登记 | `slow_start` | `{"from":"new","addr","ncpu"}` |
| 剔除 → 慢启动 | `slow_start` | `{"from":"evicted"}` |
| 急救 → 慢启动 | `slow_start` | `{"from":"rescue"}` |
| 慢启动 → 正常 | `normal` | `{}` |
| → 剔除 | `evict` | `{"from":<状态>,"reason":<原因>}` |
| 正常 → 急救 | `rescue_start` | `{"prob","candidates","timeout_count","backoff_s"}` |
| 急救结束 | `rescue_end` | `{"result":"reached"/"timeout"/...,"ms","batches_ok","batches_fail","sent","ok","timeouts","late","events","prob","target","t_before","t_after","to"}` |

- 急救结束时先发 `rescue_end`，再发随之而来的 `slow_start`/`evict`。
- `StateChange` 增加 `detail_json`（急救开始时由 `tick_states` 填入概率等），急救结束的统计由 `RescueSession` 通过新增的 `RescueEndSink` 交给 main，再转成 `rescue_end` 事件。
- `stream:events` 每条 entry 的字段：`node`、`kind`、`ts_ms`、`detail`（JSON 字符串）。`XADD ... MAXLEN ~ 10000 *`。
- `node_event` 一行：`node_id`、`ts`（毫秒精度）、`kind`、`detail`（在原 detail 上加 `"event_id": <stream id>`）。

### 4. 5 分钟聚合

- 存储线程每处理一条被接受的样本，生成一个 `MetricPoint`：node_id、接收墙上时间、cpu_busy、runq_p99（可选）、mem_avail_ratio、各网卡 rtt/超时率/重传率的平均值（没有网卡时为空）、score。
- 桶起点 `floor(ts / 300000) × 300000`（UTC 墙上时间对齐，如 10:00、10:05）。每节点每桶累计：cpu 平均与最大、runq_p99 最大、mem 最小、网络三项平均、score 平均、样本数。
- MySQL 线程每秒检查一次，把桶结束时刻 ≤ 当前时间 − 5 秒的桶取出写入（留 5 秒给迟到的样本），`INSERT ... ON DUPLICATE KEY UPDATE`（Manager 在同一桶内重启时后写的覆盖先写的：样本数少一些，但不会重复）。
- 网络取“各网卡平均”而不是最好的那块：冷数据用于回看和日报，平均更能反映整机；评分用最好网卡，`score_avg` 已经体现。没有网卡的样本不计入网络平均；整桶都没有网卡时三列写 NULL。没有调度延迟同理。
- Manager 退出时把未结束的桶也写掉（`samples` 不足 300 也写，保证不丢）。

### 5. task 表

- `TaskChange{task_id, node_id(可空), input_bytes(可空), kind, ts_wall_ms}`，kind：`allocated`、`sent`、`received`、`done`、`failed`、`timeout`、`void`。
  - 分配：Allocate 成功后（带 node_id、input_bytes）。
  - MarkSent 返回 OK 且任务存在时：`sent`。
  - OnTaskEvent：RECEIVED → `received`；DONE/FAILED → `done`/`failed`，**包括迟到的**（在途账查不到也照发，只更新表）。
  - 超时：确认超时作废 → `void`；确认超时未确认 → `timeout`；执行超时 → `timeout`。
- `TaskMerger` 按 task_id 合并 1 秒内的变化：时间列取各自第一次的值；状态按等级取较高者（allocated < sent < received < 终态），终态之后只接受 `done`/`failed` 覆盖 `timeout`（迟到的完成）。
- 每秒刷新：有分配信息的行 `INSERT ... ON DUPLICATE KEY UPDATE state=VALUES(state), t_sent=COALESCE(t_sent, VALUES(t_sent)), ...`；没有分配信息的行（分配已在之前的批次写入）`UPDATE task SET state=?, t_xxx=COALESCE(t_xxx, ?) WHERE task_id=?`。终态覆盖规则在 SQL 里同样体现：`state` 只在新状态等级更高、或新状态为 done/failed 且旧状态为 timeout 时更新（用 `CASE`）。
- 合并缓冲上限 50000 行，超出丢最旧并计数（相当于 1 秒 5 万个任务，远超规模）。

### 6. MySQL 客户端

- `MysqlClient`：`mysql_init`、`mysql_options(MYSQL_OPT_CONNECT_TIMEOUT/READ/WRITE_TIMEOUT = 2s)`、`mysql_real_connect`、`mysql_set_character_set("utf8mb4")`、`mysql_real_query`、`mysql_real_escape_string`。断线后下一次执行时重连（最多每 5 秒尝试一次）。
- 语句全部在 MySQL 线程里拼接，字符串一律转义；数值用 `%.6g` 输出，NaN/Inf 写 NULL。
- CMake：找 `mysql.h`（`mysql/` 或 `mariadb/` 子目录）和 `mysqlclient`/`mariadb` 库，找到就定义 `LM_HAVE_MYSQL`；找不到时 `mysql_client.cpp` 编为桩：`connect` 返回失败并说明“未编译 MySQL 支持”。
- 配置 `mysql.enabled`（默认 true）、`host`（127.0.0.1）、`port`（3306）、`user`（lm）、`database`（lm）；密码 `LM_MYSQL_PASSWORD`；`LM_MYSQL_HOST` 覆盖 host。

### 7. 选中概率

`tick_states` 已经遍历候选节点算总权重，顺便把每台节点的 `prob = w / Σw` 写进 `NodeState::prob`（非候选为 0）。上报时 `AcceptedSample` 带出，`node:{id}` 写 `prob`（4 位小数）。最多滞后 100 ms，看板够用。

## 性能与资源影响

- 采集、上报频率不变。
- Redis：每条事件一次 `XADD`（在存储线程的批里），很少。
- MySQL：N 台节点每 5 分钟 N 行聚合；任务按 1 秒合并（10 个/秒压测时每秒约 10–20 行）；事件很少。都在独立线程。
- 调度锁：概率计算复用已有遍历；任务变化在锁外生成，只多一次 push。
- 内存：聚合每节点一个桶（几十字节）；任务合并缓冲最多 1 秒的量。

## Risks / Trade-offs

- **MySQL 慢或挂掉**：MySQL 队列满时丢最旧（计数、限频日志），聚合桶仍在内存里，恢复后下一次刷新会写入；任务合并超过上限丢最旧。调度和 Redis 不受影响。
- **Manager 重启**：内存中未写的聚合在正常退出时写掉；崩溃会丢最多 5 分钟聚合和 1 秒任务变化。
- **事件 id 依赖 Redis**：Redis 挂掉期间的事件只进 MySQL，看板（读 stream）看不到，这与开发文档“Redis 挂掉看板暂时没有曲线”的预期一致。

## Migration Plan

- 表结构不变，无需迁移。compose 增加 `LM_MYSQL_PASSWORD`、`LM_MYSQL_HOST` 环境变量；Manager 镜像运行阶段增加 `libmariadb3`。
- 回滚：换回 `lm-manager:m6` 镜像；新写入的表数据保留无害。

## 实现中的决定（2026-11-14）

- **任务时间列取 Manager 收到时的墙上时间**，不用 `TaskEvent.ts_ms`（Worker 时钟）：与在途账的计时一致，不受节点时钟漂移影响。
- **生产者注入**：`TaskSink = std::function<void(TaskChange)>`，`CollectorService` / `SchedulerService` / `SchedulerLoop` 各有 `set_task_sink`；未设置（MySQL 关闭）时不生成。急救结束用 `RescueEndSink`，经 `RescueManager` 传给每个 `RescueSession`；main 里把它转成 `events_only` 记录推进存储队列，与随后的状态记录在同一线程依次入队，顺序有保证。
- **新节点登记与同一条上报引起的状态变化都发事件**（`CollectorService::Report` 先发 `slow_start{from:new}`，再发 `acc.change`），不会因 `changes.back()` 覆盖而丢掉登记事件。
- **未连接时的转义自己做**：mariadb-connector-c（macOS）的 `mysql_escape_string` 在没有连接时会段错误。SQL 生成测试不连库，所以 `MysqlClient::escape` 在未连接时用内置规则（`\0 \n \r \Z ' " \\`），连接后用 `mysql_real_escape_string`。
- **SQL 数值 NaN/Inf 写 `NULL`**（JSON 里写 `null`）。
- **环境变量覆盖整个 mysql 段**：除 `LM_MYSQL_HOST`、`LM_MYSQL_PASSWORD` 外，再加 `LM_MYSQL_PORT`、`LM_MYSQL_USER`、`LM_MYSQL_DATABASE`，compose 直接用 `.env` 里的 `MYSQL_PORT` 等，镜像内的配置文件不用改。
- **Manager 不 `depends_on` MySQL**：MySQL 只是冷数据，起不来时 Manager 照常运行、每 5 秒重试连接。
- `MysqlClient::query()`：取回结果集（NULL 为空串），给集成测试和以后的运维命令用。
- **Linux 也用 mariadb-connector-c**（`libmariadb-dev` / 运行库 `libmariadb3`），不用 `libmysqlclient-dev`：m7b 的 Drogon（Ubuntu `libdrogon-dev`）本身依赖 libmariadb，两个开发包互相冲突；C API 相同，macOS 本来就是 mariadb-connector-c，连 MySQL 8.0（caching_sha2_password）在两边都验证过。
- **连接断开时这一批不丢**（虚拟机验收时发现）：断开后的第一次写入才会发现连接已断（`Lost connection`），原实现已把缓冲取出，失败后这一批就丢了（实测丢 5 行任务）。改为：执行失败且连接已断开时，节点行、事件留在缓冲，任务行用 `TaskMerger::restore` 放回（与期间新到的变化按同样规则合并），已结束的桶放进重试列表，重连后先写；连接仍在（语句本身出错）时照旧丢弃并计数，避免坏数据反复重试。
- **验收时不停用户的 MySQL 容器**：“MySQL 停 60 秒”用 iptables 丢弃回环口上的 3306（`systemd-run` 3 分钟后自动撤销），效果等同于 MySQL 无响应（连接超时 2 秒、每 5 秒重试一次）。
