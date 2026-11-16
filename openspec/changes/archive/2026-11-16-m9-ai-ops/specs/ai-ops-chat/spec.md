# Spec Delta

## Purpose

自然语言运维问答：一套只读工具同时供看板问答（函数调用）和 MCP 客户端使用，回答引用真实数据，不开放任何写操作。

## ADDED Requirements

### Requirement: 只读工具集
AI 服务 SHALL 提供六个只读工具：`list_nodes()`（所有节点的状态、分数与明细、选中概率、最新指标、预测）、`get_node_metrics(node, metric, minutes)`（minutes ≤ 60 读 Redis 每秒样本，更长（≤ 1440）读 MySQL 5 分钟聚合；降采样到不超过 60 个点并给出 min/avg/max）、`get_node_events(node, hours ≤ 168)`（状态转移、急救、剔除，最多 100 条）、`explain_score(node)`（各项归一化分数、权重、总分、四个二次调整因子、权重积与选中概率）、`get_task_stats(node, hours)`（各状态任务数、完成时间 P50/P99/最大值、超时数；node 为空按节点分组）、`get_diagnoses(node, hours)`（最近诊断，最多 20 条）。工具 MUST NOT 执行任何写操作；未知节点或越界参数 SHALL 返回带 `error` 的结果而不是抛错。

#### Scenario: 解释分数
- **WHEN** 调用 `explain_score("lm-worker-1")`
- **THEN** 返回 s_cpu、s_runq、s_mem、s_net、各自权重、score、f_inflight、f_timeout、f_slow、f_pred、weight 与 prob，数值与 `node:lm-worker-1` Hash 一致

#### Scenario: 未知节点
- **WHEN** 调用 `get_node_metrics("nope", "cpu", 10)`
- **THEN** 返回 `{"error": "未知节点 nope", "nodes": [...]}`，不抛异常

### Requirement: 看板问答接口
AI 服务 SHALL 提供 `POST /api/ai/chat`，请求体 `{"messages": [{"role", "content"}, …]}`，以 SSE 流式返回事件 `tool_call`（name、args）、`tool_result`（name、ok、ms、summary）、`delta`（正文增量）、`done`（model、tokens、tool_calls）、`error`（reason）。模型 SHALL 通过函数调用使用上述工具，一轮最多 6 次工具调用，超过后强制成文；单轮总时长不超过 120 秒。LLM 未启用、缺 Key、超预算时 SHALL 立即返回 `error` 事件，reason 与诊断一致（`disabled`、`no_key`、`budget`）。token 用量计入当日预算。

#### Scenario: 一次问答
- **WHEN** 发送“现在哪台节点最闲？”
- **THEN** 先收到 `tool_call list_nodes`、`tool_result`，然后多条 `delta` 组成回答，最后 `done`；回答中提到分数或选中概率最高的节点名

#### Scenario: 未启用 LLM
- **WHEN** `ai:settings` 没选厂商时发送问题
- **THEN** 只收到一条 `error`，reason 为 `disabled`

### Requirement: MCP server
同一套工具 SHALL 以 MCP server 形式提供（stdio 传输，`ai/chat/mcp_server.py`），工具名、参数与返回和函数调用一致，配置方式与 AI 服务相同（配置文件 + 环境变量）。

#### Scenario: 客户端列工具
- **WHEN** MCP 客户端连接并请求工具列表
- **THEN** 返回六个工具及其参数 schema

### Requirement: 评估
问答正确率 SHALL 用 20 个标准答案可由脚本从 Redis/MySQL 当场算出的问题评估，记录正确率和平均工具调用次数。

#### Scenario: 运行评估脚本
- **WHEN** 运行 `experiments/chat_eval.py`
- **THEN** 输出每题是否正确、总体正确率、平均工具调用次数和平均耗时
