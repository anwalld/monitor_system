# Proposal

## Why

对应开发文档里程碑 M9（AI 第二批：运维问答 + MCP、负载预测参与调度、每日报告）。M8 之后 AI 服务只会在异常时给诊断：运维人员想问“哪台节点最闲、为什么 w1 分数低、昨天超时多少”只能自己翻 Redis 和 MySQL；调度只看当前值，负载正在爬升的节点仍按当前分数接任务；`ai_daily_report` 表和 `Advisor.PushPredictions` 接口建好了但没有东西写。M9 验收：问答正确率达标；预测优于朴素基线；日报数字一致率 100%。

## What Changes

- **运维问答与 MCP**（ai）：
  - 一套只读工具：`list_nodes`、`get_node_metrics(node, metric, minutes)`、`get_node_events(node, hours)`、`explain_score(node)`、`get_task_stats(node, hours)`、`get_diagnoses(node, hours)`，数据来自 Redis（`node:{id}`、`node:{id}:ts`、`stream:events`、`ai:pred`）和 MySQL（`node_event`、`task`、`ai_diagnosis`、`node_metrics_5m`）。
  - 同一份实现用在两处：`POST /api/ai/chat` 用 OpenAI 函数调用绑定这些工具，SSE 流式返回回答和每次工具调用；`ai/chat/mcp_server.py` 把它们注册成 MCP server（stdio），Claude Code 等客户端可直接接。
  - 一轮问答最多调 6 次工具；只提供只读工具，不开放剔除、重启等写操作。
- **负载预测参与调度**（ai、manager、proto 不改）：
  - AI 服务对每台节点用 Holt 线性指数平滑（numpy 实现，不用 LLM）每 5 秒预测 30 秒后的 CPU 忙碌度，换算成调整因子（≤ 0.7 为 1.0，0.7–1.0 线性降到 0.5，只降不升），经 `Advisor.PushPredictions` 推给 Manager（`ttl_ms = 10000`），并写 Redis `ai:pred`（10 秒过期）给看板。
  - 在线对比 Holt 与“当前值当预测”的朴素基线的平均绝对误差（MAE），`GET /api/ai/predictions` 和 health 里能看到；配置开关可关闭推送。
  - Manager 实现 `Advisor` gRPC 服务：收到预测后在节点表锁内记录因子与过期时刻，选择权重 `w = score × f_inflight × f_timeout × f_slow × f_pred` 中的 `f_pred` 取预测因子并限幅到 [0.5, 1.0]，过期当 1.0；`f_pred`、预测值进 `node:{id}` Hash 和看板节点 JSON。
- **每日报告**（ai）：每天 09:00（服务器本地时间）汇总前一天：各节点可用时长、剔除和急救次数、任务数与完成时间 P50/P99、超时数、异常与诊断摘要。数字全部由代码从 MySQL 算出存进 `stats`，LLM 只拿这些数字写正文；写完逐个检查正文里的数字都能在 `stats` 里找到，对不上只发数字表格。结果写 `ai_daily_report`；`GET /api/ai/reports`、`GET /api/ai/reports/{day}`、`POST /api/ai/reports/{day}`（手工重算）。
- **看板**（web）：新增“问答”页（对话框，流式显示回答和调用了哪些工具）和“日报”页（按日期查看）；总览卡片显示预测的 30 秒 CPU 与 `f_pred`；设置页显示预测的 MAE 对比。
- **部署**（deploy）：Nginx `/api/ai/chat` 关闭缓冲、`proxy_read_timeout` 调到 300 秒；compose 给 `ai` 加 `LM_MANAGER_ADDR`。
- **验收工具**（experiments）：`chat_eval.py`（20 个问题，标准答案由脚本从 Redis/MySQL 当场算出：正确率、平均工具调用次数）、`predict_eval.py`（用 `node:{id}:ts` 的 1 小时数据离线对比 Holt 与朴素基线的 MAE）；`mock_llm.py` 增加函数调用、流式和纯文本模式。

## Capabilities

### New Capabilities
- `ai-ops-chat`：只读运维工具集、函数调用问答（SSE）、MCP server。
- `ai-load-prediction`：Holt 预测、因子换算、推送与过期、在线基线对比。
- `ai-daily-report`：日报统计口径、LLM 成文与数字核对、存储与接口。

### Modified Capabilities
- `manager-allocation`：选择权重中的 `f_pred` 自本变更起取 `Advisor.PushPredictions` 推来的预测因子（限幅、过期）。
- `manager-hot-storage`：`node:{id}` Hash 增加 `f_pred`、`pred_cpu`。
- `manager-web-api`：节点 JSON 增加 `f_pred`、`pred_cpu`。
- `ai-service-api`：新增 `/api/ai/chat`、`/api/ai/predictions`、`/api/ai/reports*`。
- `web-dashboard`：新增问答页、日报页；总览显示预测；Nginx `/api/ai/chat` 不缓冲。

## Impact

- 对应里程碑：M9。影响模块：ai（主要）、manager（Advisor 服务、节点表、Redis 字段、节点 JSON）、web、deploy、config、experiments。proto 不改（`advisor.proto` 已在 M1 定稿）。
- **兼容性**：
  - proto：只实现已有的 `Advisor` 服务，字段号不动。Manager 新注册一个 gRPC 服务，监听端口不变（50051）。
  - Redis：`node:{id}` 新增字段 `f_pred`、`pred_cpu`（旧看板忽略即可）；新增 key `ai:pred`（Hash，10 秒过期）。不改既有 key 的格式。
  - MySQL：只写已建好的 `ai_daily_report`，读 `node_event`、`task`、`ai_diagnosis`、`node_metrics_5m`；表结构不改。
  - AI 服务接口：只新增路径，M8 的接口不变。
- 新依赖：Python `grpcio`、`grpcio-tools`（镜像构建时生成 `advisor_pb2*`）、`mcp`、`APScheduler`；看板 `marked`（渲染日报 Markdown）。
- 调度隔离：Manager 只多了一个输入（预测因子），且限幅、带过期：AI 服务停止后 10 秒内所有 `f_pred` 回到 1.0。AI 服务连不上 Manager 时只记日志，检测、诊断、问答、日报照常。
- 不在范围内：真实 LLM 问答的人工评分整理（M10 前评估时做）；预测用于 CPU 以外的指标；日报的邮件/IM 推送；按业务组的报表；写操作类工具。
