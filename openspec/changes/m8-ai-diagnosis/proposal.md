# Proposal

## Why

对应开发文档里程碑 M8（AI 第一批：异常检测 + LLM 诊断）。M7 之后每秒样本进了 `stream:metrics` 和 `node:{id}:ts`，状态转移进了 `stream:events`，`ai_diagnosis` 表也已建好，但还没有 AI 服务：

- 节点出问题时只能看到“剔除”这类事件，看不到是哪个指标、偏离多少、可能的原因。
- 开发文档要求实时检测由统计方法完成，LLM 只负责解释和成文；AI 服务整个挂掉，调度照常运行。
- M8 验收：注入的故障都能检出，诊断 JSON 合格。

## What Changes

- **AI 服务**（新增 `ai/`，Python 3.11、FastAPI，端口 8000）：
  - 用 consumer group `ai` 消费 `stream:metrics` 和 `stream:events`；启动时用 `node:{id}:ts` 预热最近 10 分钟的窗口。
  - **检测**（每秒，纯统计）：每台节点、每个指标保留最近 10 分钟窗口，用中位数和 MAD 算稳健 z 分数，|z| > 4 且连续 3 个点才算异常；剔除、开始急救、排队延迟丢弃率超过 1%、某块网卡超时率超过 20% 直接算异常；同一节点 10 分钟内只诊断一次，严重度升级除外。
  - **诊断**（异步）：组装上下文（异常、最近 10 分钟每 10 秒一个点的指标、最近的状态转移和任务超时、同一时刻其他节点的中位数），让模型按 JSON Schema 输出，用 pydantic 校验，不合格重试一次；写 `ai_diagnosis`，并往 `stream:events` 发一条 `diagnosis` 事件。诊断只给建议，不执行任何操作。
  - **云端模型接入**：openai SDK 走 OpenAI 兼容接口；`config/ai.yaml` 列出可用厂商（DeepSeek、通义千问等）和模型，不设默认厂商；看板选择存进 Redis `ai:settings`，下一次调用生效；API Key 可在看板设置页填写（BYOK，存 Redis `ai:keys`，不回显），也可放 `.env`，看板填写的优先。没选厂商、缺 Key 或超出每日 token 预算时不调用 LLM，诊断只含统计结果。调用 30 秒超时，指数退避最多重试 2 次。发给模型的只有指标和事件；节点名可配置成匿名。
  - **接口**：`GET /api/ai/health`、`GET /api/ai/diagnoses`、`GET /api/ai/diagnoses/{id}`、`GET`/`PUT /api/ai/settings`。
- **看板**：新增“诊断”页（列表、按节点和严重度筛选、详情）和“设置”页（选厂商和模型）；事件页认识 `diagnosis` 事件；节点详情显示相关诊断。
- **部署**：`deploy/docker/ai.Dockerfile`；compose 增加 `ai` 服务（host 网络）；Nginx 加 `/api/ai/` → `127.0.0.1:8000`；`.env.example` 增加各厂商 Key。
- **验收工具**：`experiments/mock_llm.py`（模拟 OpenAI 兼容接口，可注入不合格输出、超时）；`experiments/ai_eval.py`（按故障注入时刻统计检出率、检出延迟、每小时误报）。

## Capabilities

### New Capabilities
- `ai-anomaly-diagnosis`：数据来源、检测规则、诊断流程、LLM 调用约束、故障隔离。
- `ai-service-api`：AI 服务的 REST 接口和设置存储。

### Modified Capabilities
- `web-dashboard`：增加诊断页、设置页，事件页和节点详情展示诊断，Nginx 增加 `/api/ai/` 路由。

## Impact

- 对应里程碑：M8。影响模块：ai（新增），web，deploy，config，experiments。
- proto 不改；Manager 代码不改（事件页、WebSocket 已经转发 `stream:events` 里的所有事件）。
- Redis：新增 consumer group `ai`（在 `stream:metrics`、`stream:events` 上），新增 key `ai:settings`（Hash）、`ai:keys`（Hash，各厂商用户填写的 Key）、`ai:tokens:{YYYYMMDD}`（每日 token 计数，2 天过期）。`stream:events` 新增事件种类 `diagnosis`，字段格式与已有事件相同（node、kind、ts_ms、detail）。
- MySQL：只写已建好的 `ai_diagnosis`，读 `task`；表结构不改。
- 新依赖：Python 3.11、fastapi、uvicorn、redis-py、aiomysql、pydantic、openai、numpy、pyyaml、pytest。
- 调度不受影响：Manager 不读 AI 的任何输出；AI 服务停止、卡住或 LLM 超时，调度照常。
- 不在范围内：运维问答和 MCP、负载预测和 `PushPredictions`、日报（M9）；`ai:pred`（M9）；真实 LLM 的首要原因命中率人工打分（M10 前整理评估时做）。
