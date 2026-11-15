# Spec Delta

## Purpose

Manager 把节点状态转移和急救写成事件流，看板和 AI 服务从同一个流里读取。

## ADDED Requirements

### Requirement: 事件种类与内容
Manager SHALL 在以下时机生成事件 `{node, kind, ts_ms, detail}`：新节点登记（`slow_start`，detail 含 `"from":"new"`、addr、ncpu）；剔除或急救后进入慢启动（`slow_start`，`from` 为 `evicted` 或 `rescue`）；慢启动转正常（`normal`）；进入剔除（`evict`，detail 含 `from` 与 `reason`）；进入急救（`rescue_start`，detail 含 `prob`、`candidates`、`timeout_count`、`backoff_s`）；急救结束（`rescue_end`，detail 含结果、时长、成功与失败批数、请求数、成功、超时、迟到、网络事件数、最终概率与目标、T 的前后值、转入的状态）。急救结束时 `rescue_end` SHALL 先于随后的 `slow_start` 或 `evict`。

#### Scenario: 剔除事件
- **WHEN** w1 因失联被剔除
- **THEN** 生成一条 kind 为 `evict`、detail 含 `"reason":"lost"` 的事件

#### Scenario: 急救事件顺序
- **WHEN** w1 急救结束并进入慢启动
- **THEN** 先有一条 `rescue_end`，紧接着一条 `slow_start`（`from` 为 `rescue`）

### Requirement: 写入 stream:events
每条事件 SHALL 以 `XADD stream:events MAXLEN ~ 10000 *` 写入，字段为 `node`、`kind`、`ts_ms`、`detail`（JSON 字符串）。写入失败只记日志。

#### Scenario: 读取事件
- **WHEN** w1 进入剔除
- **THEN** `XREVRANGE stream:events + - COUNT 1` 返回的 entry 中 `node` 为 `w1`、`kind` 为 `evict`
