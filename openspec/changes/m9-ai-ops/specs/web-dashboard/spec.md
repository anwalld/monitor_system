# Spec Delta

## ADDED Requirements

### Requirement: 问答页
看板 SHALL 提供问答页：输入问题后流式显示回答，并显示本轮调用了哪些工具（名称、参数、结果概括、耗时）；LLM 未启用时提示到设置页选择厂商；AI 服务不可用时显示“AI 服务不可用”。对话历史保存在页面内，随请求发送最近 10 轮。

#### Scenario: 提问
- **WHEN** 输入“lm-worker-1 为什么分数低？”并发送
- **THEN** 页面先出现“调用 explain_score”等工具条目，随后回答逐字出现

### Requirement: 日报页
看板 SHALL 提供日报页：列出已有日期（`GET /api/ai/reports`），选中后显示 Markdown 正文和数字表格；正文被核对否决时只显示表格并注明原因；提供“重新生成”按钮。

#### Scenario: 查看日报
- **WHEN** 选择某一天
- **THEN** 显示该天各节点的可用率、剔除/急救次数、任务 P50/P99、超时数

### Requirement: 预测展示
总览卡片 SHALL 显示每台节点预测的 30 秒后 CPU 与 `f_pred`（无预测显示 —）；设置页 SHALL 显示预测是否启用、是否推送，以及 Holt 与朴素基线最近 1 小时的 MAE。

#### Scenario: 预测生效
- **WHEN** 某节点 `f_pred` 为 0.75
- **THEN** 总览卡片显示 `f_pred 0.75` 并以警示色标出

## MODIFIED Requirements

### Requirement: Nginx 入口
Nginx SHALL 把 `/api/ai/chat` 反代到 AI 服务并关闭代理缓冲、`proxy_read_timeout` 不少于 300 秒（SSE 逐事件下发）；其余 `/api/ai/` 反代到 AI 服务（端口 8000），`/api/` 与 `/ws` 反代到 Manager 的 HTTP 端口（WebSocket 升级），`/` 提供看板静态文件。

#### Scenario: 经 Nginx 访问
- **WHEN** 浏览器打开 `http://<manager>/` 并在问答页提问
- **THEN** 回答逐字出现，不是等全部生成后一次出现
