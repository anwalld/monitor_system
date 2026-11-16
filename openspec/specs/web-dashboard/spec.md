# web-dashboard Specification

## Purpose
Web 看板实时展示节点状态、历史曲线和事件，经 Nginx 统一入口访问。

## Requirements

### Requirement: 页面
看板 SHALL 提供总览（每台节点一张卡片：状态、分数、选中概率、CPU、调度延迟 P99、内存、各网卡状态）、节点详情（最近 1 小时 CPU/内存/磁盘、调度延迟、网卡曲线，分数明细，该节点事件时间线）、事件页（全部事件，可按节点和种类筛选）。

#### Scenario: 总览实时更新
- **WHEN** 某节点被剔除
- **THEN** 总览卡片 1 秒内显示剔除状态，事件页出现一条 `evict`

### Requirement: 断线重连与去重
连接断开后看板 SHALL 按 1、2、4、8、16、30 秒退避重连（之后保持 30 秒，连上后归零），重连时带 `since=<收到的最大事件 id>`，并按事件 id 去重。

#### Scenario: Manager 重启
- **WHEN** Manager 重启，期间产生新节点登记事件
- **THEN** 看板自动重连，事件页包含重启期间的事件，每条只出现一次

### Requirement: Nginx 入口
Nginx SHALL 把 `/api/ai/chat` 反代到 AI 服务并关闭代理缓冲、`proxy_read_timeout` 不少于 300 秒（SSE 逐事件下发）；其余 `/api/ai/` 反代到 AI 服务（端口 8000），`/api/` 与 `/ws` 反代到 Manager 的 HTTP 端口（WebSocket 升级），`/` 提供看板静态文件。

#### Scenario: 经 Nginx 访问
- **WHEN** 浏览器打开 `http://<manager>/` 并在问答页提问
- **THEN** 回答逐字出现，不是等全部生成后一次出现

### Requirement: 诊断页
看板 SHALL 提供诊断页：按时间倒序列出 AI 诊断（时间、节点、严重度、一句话结论、触发指标、模型），可按节点和严重度筛选，点开显示可能原因（含证据和置信度）、建议操作和异常详情；收到 `diagnosis` 事件时实时插入。

#### Scenario: 实时出现
- **WHEN** AI 服务写入一条诊断
- **THEN** 诊断页不刷新就出现这一条

### Requirement: 设置页
看板 SHALL 提供设置页：列出 `GET /api/ai/settings` 返回的厂商和模型，标出 Key 是否已配置及来源，提供密码框填写所选厂商的 Key（留空不修改，可删除已填的 Key），保存时调用 `PUT /api/ai/settings`；可以选择“不启用”。

#### Scenario: 切换模型
- **WHEN** 在设置页选择另一个模型并保存
- **THEN** `ai:settings` 更新，下一条诊断的 `model` 是新模型

### Requirement: 事件页与节点详情展示诊断
事件页 SHALL 把 `diagnosis` 事件显示为“AI 诊断”，摘要为严重度和一句话结论；节点详情 SHALL 列出该节点最近 24 小时的诊断。

#### Scenario: AI 服务不可用
- **WHEN** AI 服务停止
- **THEN** 诊断页和设置页显示“AI 服务不可用”，总览、节点详情的其他部分和事件页正常

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
总览卡片 SHALL 显示每台节点预测的 30 秒后 CPU：预测参与调度时（Manager 节点 JSON 有 `pred_cpu`）同时显示 `f_pred`，`f_pred` < 1 时以警示色标出；推送关闭时显示 AI 服务算出的预测（`GET /api/ai/predictions`）并注明“未参与调度”；都没有时显示 —。设置页 SHALL 显示预测是否启用、是否推送，以及 Holt 与朴素基线最近 1 小时的 MAE。

#### Scenario: 预测生效
- **WHEN** 推送开启，某节点 `f_pred` 为 0.75
- **THEN** 总览卡片显示 `f_pred 0.75` 并以警示色标出

#### Scenario: 推送关闭
- **WHEN** `predict.push` 为 false，AI 服务正常
- **THEN** 总览卡片显示 AI 算出的预测 CPU 和“未参与调度”，`f_pred` 不显示
