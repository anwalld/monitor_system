# Proposal

## Why

对应开发文档里程碑 M7 的前半部分。到 M6 为止，Manager 的数据只在 Redis 里保留 1 小时，状态转移、急救只写日志：

- 看板（M7 后半）需要一个事件流来实时推送状态变化，断线重连时还要能补发；AI 服务（M8）也要从同一个流里消费事件。
- 开发文档的冷数据（`node_metrics_5m`、`node_event`、`task`、`node` 表）一直没有写入，日报（M9）和对照实验（M10）都要用。
- `node:{id}` 里还缺“选中概率”，看板总览卡片要显示它。

M7 拆成两个变更：`m7a-storage-events`（本变更：事件流、MySQL 冷数据、选中概率）和 `m7b-dashboard`（Drogon REST + WebSocket、Vue 看板、Nginx）。

## What Changes

- **事件**：节点状态转移和急救开始、结束都生成一条事件 `{node, kind, ts_ms, detail}`：
  - kind：`slow_start`、`normal`、`evict`、`rescue_start`、`rescue_end`（与开发文档 `node_event.kind` 一致）。
  - 存储线程 `XADD stream:events MAXLEN ~ 10000`；同时写 MySQL `node_event`。
- **MySQL 写线程**（`manager/storage/mysql_writer`）：独立线程和队列，Redis 写入不受 MySQL 拖累。
  - `node`：新节点登记时插入，地址、核数变化时更新。
  - `node_metrics_5m`：按节点在内存中累计 5 分钟（按墙上时间对齐），桶结束后批量写入一行。
  - `node_event`：事件批量插入。
  - `task`：在途账的每一步（分配、MarkSent、RECEIVED、完成、超时、作废，以及迟到的完成）先在内存合并 1 秒，再批量写入；迟到的完成只更新 `task` 表。
  - 写失败只记日志（限频），不重试、不积压超过上限。
- **选中概率**：调度主循环每 100 ms 计算每台候选节点的选中概率，`node:{id}` 增加 `prob` 字段。
- **配置**：`manager.yaml` 增加 `mysql` 段（主机、端口、用户、库名、批量与上限）；密码从环境变量 `LM_MYSQL_PASSWORD` 读取。compose 把 `MYSQL_PASSWORD` 传给 manager。

## Capabilities

### New Capabilities
- `manager-events`：事件的种类、字段、生成时机，写入 `stream:events` 的格式与长度上限。
- `manager-cold-storage`：MySQL 写线程，`node`、`node_metrics_5m`、`node_event`、`task` 四张表的写入规则、聚合口径、批量与失败处理。

### Modified Capabilities
- `manager-hot-storage`：`node:{id}` 增加 `prob`。以 ADDED 增量写。

## Impact

- 对应里程碑：M7（前半）。影响模块：manager（`storage/` 新增 MySQL 写线程与事件，`ingest/node_table` 算概率并输出任务变化，`api/`、`scheduler_loop`、`main.cpp` 接线）、deploy（compose 环境变量、Manager 镜像运行库）、config、tests。
- proto 不改。
- Redis：新增 `stream:events`（每条事件一条，MAXLEN 约 1 万）；`node:{id}` 多一个字段。
- MySQL：表结构已在 M0 建好，不改。`task.task_id` 是 `CHAR(26)`，开发文档写的是 ULID，本项目的 task_id（m5a 定义，如 `19a7c3e2f10-42`）不超过 26 字符，直接存，不改格式。写入量：每节点每 5 分钟一行聚合；`task` 每个任务 1–2 次写（按 1 秒合并）；事件很少。
- 调度锁：概率计算放在 `tick_states` 已有的遍历里（每 100 ms，≤ 20 台）；任务变化在锁外生成。
- 新依赖：MySQL C 客户端库（Ubuntu `libmariadb-dev`，macOS `mariadb-connector-c`；最初计划用 `libmysqlclient-dev`，因与 Drogon 的依赖冲突改掉，见 design）。开发文档写的是 MySQL Connector/C++，但 Ubuntu 24.04 的包是 2014 年的 1.1 版（旧 JDBC 风格 API），与 Homebrew 的 9.x 不兼容；C API 在两边一致，所以改用 C API 加一层很薄的封装。找不到库时 MySQL 写线程编为桩（只记一条警告），其余功能不受影响。
- 不在范围内：
  - Drogon 接口、WebSocket、看板、Nginx（m7b）。
  - `ai_diagnosis`、`ai_daily_report` 表（M8、M9）。
  - 节点下线时删除 `node:{id}`：M6 起节点表只增不删（剔除的节点仍保留状态），本变更也不删。
