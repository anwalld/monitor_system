# Spec Delta

## Purpose

Manager 通过 REST 和 WebSocket 向看板提供节点实时状态、历史曲线和事件，断线重连时补发错过的事件。

## ADDED Requirements

### Requirement: 独立的接口线程
Manager SHALL 在独立线程中运行 HTTP 服务（默认 `0.0.0.0:8080`），不与 gRPC、调度主循环、存储线程共用线程。Redis 不可用时 `/api/nodes` 与 `node_update` SHALL 照常返回（数据来自内存），曲线与事件接口返回空或 503；调度 MUST NOT 受影响。

#### Scenario: Redis 停止
- **WHEN** Redis 停止
- **THEN** `GET /api/nodes` 仍返回全部节点，Allocate 正常

### Requirement: 节点接口
`GET /api/nodes` SHALL 返回 `{"ts","nodes":[…]}`，每个节点含 id、addr、ncpu、online、state、score 与分数明细、prob、cpu、mem、disk、runq_p99_us（未上报为 null）、在途信息、各网卡指标。`GET /api/nodes/{id}` 返回单个节点，不存在返回 404。

#### Scenario: 列表
- **WHEN** 两台节点在线
- **THEN** 返回两项，`prob` 之和约为 1

### Requirement: 曲线接口
`GET /api/nodes/{id}/series?minutes=M&step_s=K`（默认 60、5）SHALL 从 `node:{id}:ts` 取最近 M 分钟，按 K 秒分桶：cpu、mem、disk、rx/tx 取平均，`runq_p99_us` 取最大，网络三项先对网卡平均再在桶内平均；空桶不输出。

#### Scenario: 1 小时降采样
- **WHEN** 节点连续上报 1 小时，请求默认参数
- **THEN** 返回约 720 个点，时间间隔 5 秒

### Requirement: 事件接口
`GET /api/events?limit=N`（默认 200，最多 1000）SHALL 按时间倒序返回 `stream:events` 中的事件；`GET /api/nodes/{id}/events?limit=N` 只返回该节点的事件。每条事件含 id（Stream entry id）、node、kind、ts、detail（JSON 对象）。

#### Scenario: 节点事件
- **WHEN** w1 被剔除后恢复
- **THEN** `/api/nodes/w1/events` 最前面依次是 `slow_start`（from evicted）与 `evict`

### Requirement: WebSocket 推送
`/ws` 连接建立后 SHALL 先发 `snapshot`（含 `last_event_id` 与全部节点），之后每秒发 `node_update`，`stream:events` 有新事件时立即发 `event`。带 `?since=<id>` 时 SHALL 先补发 `stream:events` 中 id 大于 since 的全部事件（最多 10000 条），再发 `snapshot`。连接 SHALL 在读取补发之前加入广播集合，保证补发与实时推送之间不遗漏。

#### Scenario: 断线补发
- **WHEN** 客户端断开期间产生了 3 条事件，客户端带上次最后的事件 id 重连
- **THEN** 收到这 3 条事件，随后收到 `snapshot`

### Requirement: 静态 token
配置了 token（`api.token` 或 `LM_API_TOKEN`）时，除 `/api/health` 外的请求 SHALL 携带 `Authorization: Bearer <token>`（WebSocket 也可用 `?token=`），否则返回 401 或拒绝连接。未配置时 SHALL 放行并在启动时警告。

#### Scenario: 缺少 token
- **WHEN** 配置了 token，请求 `/api/nodes` 不带 token
- **THEN** 返回 401
