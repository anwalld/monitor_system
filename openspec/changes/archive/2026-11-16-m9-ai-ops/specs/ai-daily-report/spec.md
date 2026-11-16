# Spec Delta

## Purpose

每日报告：代码从 MySQL 算出前一天的数字，LLM 只负责成文，正文里每个数字都能在统计里找到。

## ADDED Requirements

### Requirement: 统计口径
日报 SHALL 按服务器本地时间的自然日统计前一天：每台节点的可用时长（当天 `node_metrics_5m.samples` 之和，秒）与可用率、CPU 平均/最大、剔除次数（`evict` 事件）、急救次数（`rescue_start` 事件）、任务数与各状态计数、完成时间 P50/P99/最大值（`t_done − t_recv`，毫秒，只算 done）、超时数（分为执行超时和确认超时：t_recv 为空的 timeout 是分配后 Worker 没确认收到）；全局合计；诊断按严重度计数、按节点计数和摘要列表；当天诊断按触发指标的计数（被去重而没有诊断的异常只在 AI 服务内存里，不计入）。所有数字由代码算出存进 `ai_daily_report.stats`。

#### Scenario: 一天的数据
- **WHEN** 某节点当天有 288 行 5 分钟聚合、samples 之和 86100，`evict` 事件 3 条，`task` 表 1200 行其中 10 行 timeout
- **THEN** stats 里该节点 `avail_s = 86100`、`avail_pct = 99.65`、`evictions = 3`、`tasks = 1200`、`timeouts = 10`

### Requirement: 成文与核对
LLM 启用时 SHALL 只拿 stats 写 Markdown 正文；正文中出现的每个数字（剔除日期和时间）SHALL 能在 stats 的数值集合（允许取整、1–2 位小数、百分数、毫秒与秒的换算）里找到，否则 SHALL 丢弃正文、改用代码生成的数字表格，并在 stats 里记录否决原因和不匹配的数字。LLM 未启用或调用失败时同样使用数字表格。

#### Scenario: 正文多写了一个数
- **WHEN** LLM 正文写“超时率 0.83%”而 stats 里没有这个数
- **THEN** `ai_daily_report.markdown` 为数字表格，`stats.llm = "mismatch"`，`stats.mismatched` 含 `0.83`

#### Scenario: 全部一致
- **WHEN** LLM 正文的数字都来自 stats
- **THEN** 保存 LLM 正文，`stats.llm = "ok"`

### Requirement: 生成时机与接口
AI 服务 SHALL 每天 `report.at`（默认 09:00，服务器本地时间）生成前一天的日报，写入 `ai_daily_report`（同一天重复生成覆盖）；启动时若前一天的日报缺失且已过 `report.at`，补生成一次。SHALL 提供 `GET /api/ai/reports?limit=30`（日期列表，新的在前）、`GET /api/ai/reports/{day}`（stats 与 markdown，不存在返回 404）、`POST /api/ai/reports/{day}`（立即重算该日并返回结果）。

#### Scenario: 手工重算
- **WHEN** 请求 `POST /api/ai/reports/2026-11-15`
- **THEN** `ai_daily_report` 里 2026-11-15 的行被写入或覆盖，响应包含 stats 和 markdown

#### Scenario: 看板查看
- **WHEN** 打开看板日报页
- **THEN** 列出已有日期，点开显示正文与数字表格
