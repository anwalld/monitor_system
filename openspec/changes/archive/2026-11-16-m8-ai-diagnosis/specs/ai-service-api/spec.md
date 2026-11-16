# Spec Delta

## Purpose

AI 服务的 REST 接口：诊断查询和 LLM 厂商设置。

## ADDED Requirements

### Requirement: 诊断查询
AI 服务 SHALL 提供 `GET /api/ai/diagnoses`（参数 node、severity、hours，默认 24，limit 默认 100，最大 1000，按时间倒序）和 `GET /api/ai/diagnoses/{id}`（含完整 anomaly 和 report），数据来自 `ai_diagnosis`。

#### Scenario: 按节点筛选
- **WHEN** 请求 `GET /api/ai/diagnoses?node=lm-worker-1`
- **THEN** 只返回该节点的诊断

### Requirement: 最近异常
AI 服务 SHALL 提供 `GET /api/ai/anomalies`，返回内存中最近 1000 个检测器异常（包括因去重没有诊断的），可用 `since_ms` 过滤，用于评估检出率和误报。

#### Scenario: 被去重的异常
- **WHEN** 同一节点 10 分钟内第二次出现同等严重度的异常
- **THEN** 没有新诊断，但 `/api/ai/anomalies` 里能看到这次异常，标记为已去重

### Requirement: 厂商设置
AI 服务 SHALL 提供 `GET /api/ai/settings`，返回 `config/ai.yaml` 列出的厂商（名称、显示名、可选模型、Key 是否已配置、来源 ui/env、末 4 位提示，不返回 Key 本身）和当前选择；`PUT /api/ai/settings` 接收 `{"vendor", "model", "api_key"?, "clear_key"?}`，校验厂商和模型在列表里后写入 Redis `ai:settings`，下一次 LLM 调用生效。vendor 为空表示关闭 LLM。用户可以在看板填写自己的 Key（BYOK）：`api_key` 非空时保存到 Redis `ai:keys`（按厂商），`clear_key` 删除；看板填写的 Key 优先于 `.env`。

#### Scenario: 选择未列出的模型
- **WHEN** PUT 的 model 不在该厂商的可选列表里
- **THEN** 返回 400，`ai:settings` 不变

#### Scenario: 填写的 Key 不回显
- **WHEN** 在设置页填写 DeepSeek 的 Key 后请求 `GET /api/ai/settings`
- **THEN** 响应里只有 `key_configured`、`key_source = ui` 和末 4 位提示，没有完整 Key

### Requirement: 健康与鉴权
AI 服务 SHALL 提供 `GET /api/ai/health`（Redis、MySQL 连接状态，LLM 是否启用，当日 token 用量，检测的节点数，诊断队列长度），不鉴权。配置了 `API_TOKEN` 时其余接口 SHALL 要求与 Manager 相同的 token（`Authorization: Bearer` 或 `token=`）。

#### Scenario: 健康检查
- **WHEN** MySQL 不可用
- **THEN** `/api/ai/health` 仍返回 200，`mysql` 为 false
