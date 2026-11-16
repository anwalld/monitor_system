# Design

## Context

见 proposal.md 的 Why。现状：

- AI 服务（`ai/lmai`）是一个 uvicorn 进程，lifespan 里起 4 个 asyncio 任务（metrics 消费、events 消费、诊断、MySQL 补写）；`LlmClient` 只有面向诊断的 JSON 调用（`diagnose`），没有工具调用和流式；`Redis`/`MySQL` 封装只有诊断要的几个查询。
- Manager 选择权重已经是 `score × f_inflight × f_timeout × f_slow × f_pred`，`Factors::pred` 固定为 1；急救触发条件里已经把 `f.pred` 算进去（`f_timeout` 必须是最小的因子才急救）。`Advisor` 服务从 M1 起只有生成代码，没有注册。
- `node:{id}` Hash 已有分数明细（`s_cpu`、`s_runq`、`s_mem`、`s_net`、`best_nic`）与因子（`f_inflight`、`f_timeout`、`f_slow`、`prob`）；权重（0.3/0.3/0.2/0.2）只在 `manager.yaml`。
- MySQL `task` 表有 `t_recv`、`t_done` 和 `state`；`node_metrics_5m` 每节点每 5 分钟一行，`samples` 列是桶内样本数；`node_event` 存全部状态转移（含 `evict`、`rescue_start`）；`ai_daily_report(day, stats JSON, markdown, created_at)` 已建。
- Nginx 对 `/api/ai/` 开着代理缓冲、`proxy_read_timeout 60s`，SSE 会被攒着不下发。

## Goals / Non-Goals

**Goals：**
- 工具实现一份、两处复用（函数调用与 MCP），都是只读。
- 预测走旁路：Manager 侧限幅 + 过期，AI 停掉 10 秒内恢复原权重；锁内只做几次比较和赋值。
- 日报正文里的每个数字都能回溯到代码算出的 `stats`。
- 不改 proto、不改表结构、不改 M8 的接口。

**Non-Goals：**
- 不做多轮对话的服务端持久化（历史由看板保存并随请求发来）。
- 不做预测模型的自动调参和自动开关（在线 MAE 对比只用于人工决定）。
- 不做报告推送（邮件、IM）。

## Decisions

### 1. 问答：OpenAI 函数调用 + SSE，流式解析 tool_calls

- `POST /api/ai/chat`，请求 `{"messages": [{"role": "user"|"assistant", "content": "…"}, …]}`，看板只发最近 10 轮。响应 `text/event-stream`，事件：
  - `tool_call`：`{"id", "name", "args"}`，模型决定调用某个工具时立刻发；
  - `tool_result`：`{"id", "name", "ok", "ms", "summary"}`，`summary` 是结果的一行概括（条数、节点数）；
  - `delta`：`{"text"}`，回答正文增量；
  - `done`：`{"model", "tokens", "tool_calls", "ms"}`；
  - `error`：`{"reason"}`，`reason` 与诊断一致（`disabled`、`no_key`、`budget`、`error: …`）。
- 每一轮 LLM 请求都用 `stream=True`：正文增量直接转发；`tool_calls` 的增量按 `index` 拼接 `id`、`name`、`arguments`，`finish_reason == "tool_calls"` 时执行。这样第一个字就能上屏，不必为了拿完整的 tool_calls 退回非流式。
- 一轮最多 6 次工具调用（`chat.max_tool_calls`）；达到上限后最后一次请求带 `tool_choice="none"`，强制成文。单次问答总时长上限 120 秒。
- token 统计：请求带 `stream_options={"include_usage": true}`（DeepSeek、通义千问都支持）；没有 usage 时按字符数 / 3 估算。计入 `ai:tokens:{日期}`，预算用尽时返回 `error: budget`。
- 工具结果以 JSON 字符串放进 `role: tool` 消息，单个结果超过 8 KB 时截断序列（保留首尾）并注明 `truncated`。
- 系统提示词复用诊断提示词里的指标含义和阈值说明，另加：先调工具再回答、引用数值带指标名、不要编造工具没返回的数据、只读、用中文。节点名匿名化（`diagnosis.anonymize`）同样适用：工具参数里的代号先换回真名再查询，结果再匿名化。

**备选**：非流式 + 一次性返回 JSON——实现简单但回答要等 10–30 秒，开发文档明确要求 SSE。

### 2. 工具集：`ai/lmai/tools.py`，一个类，六个方法，返回 dict

| 工具 | 数据来源 | 说明 |
|---|---|---|
| `list_nodes()` | `SCAN node:*`（排除 `:ts`）→ `HGETALL` | 每台：id、state、state_reason、score、s_*、prob、cpu_busy、mem_avail_ratio、runq_p99_us、评分因子、最后上报距今秒数、`ai:pred` 里的预测 |
| `get_node_metrics(node, metric, minutes)` | `minutes ≤ 60`：`node:{id}:ts`；更长（≤ 1440）：`node_metrics_5m` | metric：`cpu`、`mem_avail`、`disk`、`runq_p99_us`、`rtt_ms`、`timeout_rate`、`retrans_rate`、`rx_bps`、`tx_bps`（网卡指标按网卡分列）。降采样到不超过 60 个点：步长 = ceil(minutes × 60 / 60) 秒，桶内平均，`runq_p99_us` 取最大。返回 `start`、`step_s`、`points`（本地时间）、`min/avg/max` |
| `get_node_events(node, hours)` | MySQL `node_event`；MySQL 不可用时退到 `stream:events` | 最多 100 条，新的在前，含 kind、ts、detail（reason、from/to…）。`hours ≤ 168` |
| `explain_score(node)` | `node:{id}` Hash + `ai.yaml` 的 `chat.score_weights` | 各项归一化分数、权重、`score = Σ w×s`、四个因子、`weight = score × 因子`、prob、剔除状态；附一句规则说明（取对数的 runq、网络取最好网卡） |
| `get_task_stats(node, hours)` | MySQL `task` | 按 state 计数；`done` 的 `t_done − t_recv` P50/P99/max（ms）；超时数；node 为空时按节点分组 |
| `get_diagnoses(node, hours)` | MySQL `ai_diagnosis` | 最多 20 条：ts、severity、summary、首要原因、model |

- `chat.score_weights` 默认与 `manager.yaml` 一致（cpu 0.3、runq 0.3、mem 0.2、net 0.2），配置里注明“与 manager.yaml 保持一致”。Manager 不把权重写进 Redis（范围外），所以这里是一份镜像。
- 工具参数用 JSON Schema 描述（供函数调用和 MCP 共用），参数校验：node 必须在 `list_nodes` 的集合里（否则返回 `error: 未知节点` 让模型改正），minutes/hours 限幅。
- 时间输出用服务器本地时间 ISO 字符串（容器挂了 `/etc/localtime`）。

### 3. MCP server：`ai/chat/mcp_server.py`，stdio，FastMCP

- 用官方 `mcp` SDK 的 `FastMCP`，对每个工具注册同名函数（docstring = 工具说明，签名 = 参数）；函数体直接调用 `lmai.tools.Tools`。
- 配置和 AI 服务相同（`LM_AI_CONFIG`、`LM_REDIS_*`、`LM_MYSQL_*` 环境变量）；作为独立进程运行：`python -m chat.mcp_server`（镜像里 `/app`）或 `docker run -i lm-ai python -m chat.mcp_server`。
- 不走 HTTP，不鉴权（本机 stdio）。只注册只读工具。

### 4. 负载预测：Holt 线性，每节点一份状态，AI 进程内每 5 秒推送

- 输入：metrics 消费任务每收到一条样本，调用 `Predictor.observe(node, ts_ms, cpu_busy)`（在检测之后，同一协程，纯内存，微秒级）。
- Holt：`level' = α x + (1 − α)(level + trend)`，`trend' = β (level' − level) + (1 − β) trend`；预测 `ŷ(h) = level + h × trend`，h = 30（秒），结果限幅 [0, 1]。初值：第一个样本 level = x、trend = 0；少于 `min_samples`（默认 30）不输出预测。α、β 默认 0.3、0.1（可配，`predict_eval.py` 可以扫参数）。样本间隔不是严格 1 秒时按秒数折算 h。
- 因子：`f = 1.0`（ŷ ≤ 0.7）；`f = 1 − 0.5 × (ŷ − 0.7) / 0.3`（0.7 < ŷ < 1.0）；`f = 0.5`（ŷ ≥ 1.0）。只降不升：f ≤ 1。
- 预测任务（第 5 个 asyncio 任务）每 5 秒：收集最后样本不超过 10 秒的节点，先写 `ai:pred`（`DEL` + `HSET node → {"cpu_busy_30s", "factor", "cpu_now", "ts_ms"}` + `EXPIRE 10`，只要 `predict.enabled`）；`predict.push` 也开着时再组 `PredictionBatch{ts_ms, ttl_ms: 10000, items}`，`grpc.aio` 调 `Advisor.PushPredictions`，超时 2 秒；失败限频记日志，不重试，下一轮再推。
- 在线评估：每次预测记 `(node, target_ts = ts + 30 s, ŷ, naive = x)`；样本到达时取最近到期的记录，累计 `|ŷ − x|` 与 `|naive − x|`，滚动窗口 1 小时（按节点和总计）。`GET /api/ai/predictions` 返回每台节点最新预测、因子、1 小时 MAE（holt、naive、点数）；health 加 `predict: {enabled, push, nodes, mae_holt, mae_naive}`。
- 开关：`predict.enabled`（计算与评估）、`predict.push`（推给 Manager；离线评估比不过基线时关掉）。都为 true 时才推送。
- 影响：Redis 每 5 秒多一个 HSET+EXPIRE；Manager 每 5 秒多一个 gRPC 调用；锁持有时间：每台节点 3 次赋值。

**备选**：Manager 侧自己做 Holt——不需要 AI 进程，但开发文档把预测放在 AI 服务（AI 挂掉调度照常，预测也应随 AI 一起挂掉），且 Manager 侧要再加评估逻辑。

### 5. Manager：`Advisor` 服务与 `f_pred`

- `manager/api/advisor_service.{h,cpp}`：`PushPredictions` 校验 `node_id` 非空（否则 `INVALID_ARGUMENT`）；`factor` 非有限时当 1.0、`cpu_busy_30s` 非有限时当 0（不因一条坏数据拒绝整批），`cpu_busy_30s` 限幅到 [0, 1]；`ttl_ms` 为 0 时用默认 10000，上限 `advisor.max_ttl_ms` 60000；然后 `NodeTable::set_predictions(items, ttl_ms, now_mono_ms)` 一次加锁：对每个已知节点写 `pred_factor`（限幅到 [`advisor.min_factor`, 1.0]）、`pred_cpu`、`pred_until_mono_ms`。未知节点忽略、计数，返回 OK（不让 AI 侧因为一台刚下线的节点整批失败）。`advisor.enabled = false` 时服务仍注册但直接返回 OK、不写（方便 AI 侧不改配置就能关掉）。
- `factors_for`：`f.pred = now ≤ pred_until ? pred_factor : 1.0`。急救触发条件沿用（`f_timeout` 必须 ≤ `f_pred`）。
- `AcceptedSample`、`NodeView` 增加 `f_pred`、`pred_cpu`（无预测或过期时 `f_pred = 1`、`pred_cpu` 为空）；Redis Hash 字段 `f_pred`、`pred_cpu`（过期或无预测时 HDEL `pred_cpu`）；`node_json` 增加 `f_pred`、`pred_cpu`（null）。
- 配置：`manager.yaml` 新增 `advisor: {enabled: true, min_factor: 0.5, default_ttl_ms: 10000, max_ttl_ms: 60000}`。
- 不新开线程：gRPC 线程池处理，锁内无 IO。

### 6. 日报：代码算数、LLM 成文、逐数核对

- 日界：服务器本地时间的自然日（容器挂 `/etc/localtime`）；MySQL 列是 UTC，查询时把本地 00:00–24:00 换成 UTC 区间。
- `stats`（全部由 SQL/代码算出）：
  ```json
  {"day": "2026-11-15", "tz": "CST", "day_s": 86400,
   "nodes": [{"node": "lm-worker-1", "avail_s": 85999, "avail_h": 23.89, "avail_pct": 99.54,
              "cpu_avg_pct": 40.0, "cpu_max_pct": 99.0, "evictions": 24, "rescues": 0,
              "tasks": 2814, "done": 1340, "failed": 0, "timeouts": 1472, "exec_timeouts": 0, "unconfirmed": 1472,
              "timeout_pct": 52.31, "p50_ms": 201, "p99_ms": 206, "max_ms": 344}],
   "totals": {"nodes": 2, "tasks": 7913, "done": 3872, "failed": 0, "timeouts": 4038, "exec_timeouts": 0,
              "unconfirmed": 4038, "timeout_pct": 51.03, "p50_ms": 203, "p99_ms": 209, "max_ms": 344,
              "evictions": 37, "rescues": 0, "states": {"done": 3872, "timeout": 4038, "void": 3}},
   "diagnoses": {"total": 63, "high": 30, "medium": 31, "low": 2, "by_node": {…}, "llm_ok": 60,
                 "items": [{"time": "09:12", "node": "…", "severity": "high", "summary": "…"}]},
   "triggers": [{"trigger": "调度延迟 P99（µs）", "count": 29}],
   "llm": "ok", "model": "deepseek/deepseek-chat", "tokens": 3472}
  ```
  - `avail_s` = 当天 `node_metrics_5m.samples` 之和（Worker 每秒上报一次，样本数即上报秒数），不超过当天秒数；`avail_pct = avail_s / day_s × 100`，`avail_h` 为小时。
  - CPU 平均按样本数加权，和最大值一样直接给百分数（`_pct`），免得模型自己换算。
  - 剔除、急救 = `node_event` 当天 `kind = evict` / `rescue_start` 的行数。
  - 任务 = `task` 表按 `t_alloc` 落在当天：各 state 计数；`timeout` 按 `t_recv` 是否为空拆成 `unconfirmed`（确认超时）和 `exec_timeouts`（执行超时）；完成时间 = `t_done − t_recv`（毫秒，只算 `done`），P50/P99 取最近秩分位。
  - 诊断 = `ai_diagnosis` 当天的行；`triggers` 按诊断的触发指标计数（检测器被去重的异常只在内存里，不计入）。
  - 核对被否决时另存 `mismatched`（对不上的数字）和 `llm_text`（被否决的正文）。
- LLM：系统提示词要求“只用给出的数字，不要算新数字（百分比、差值）；用中文；Markdown；200–400 字；先总体再异常再建议”；用户消息 = `stats` JSON。不用 JSON 模式，`max_tokens 1500`。
- 核对：从正文里提取所有数字（整数、小数、带 % 的）；允许集合 = `stats` 里所有数值的多种写法（原值、四舍五入到 0/1/2 位、百分数形式、`p50_ms` 等的秒表示）。有任何数字不在允许集合里 → 不用正文，改为代码生成的 Markdown 表格，并在 `stats.llm` 记 `mismatch` 与不匹配的数字列表；LLM 未启用/失败同样落到表格（`stats.llm = disabled | no_key | budget | error: …`）。年份、日期里的数字（`2026-11-15`、`09:00`）在提取时先剔除。
- 调度：APScheduler `AsyncIOScheduler`，cron `report.at`（默认 `09:00`）生成“昨天”；启动时若昨天的报告不存在且当前时刻已过 `report.at`，补生成一次（补的也只算昨天，不回填更早的）。`POST /api/ai/reports/{day}` 立即重算任意一天（验收用，鉴权同其他接口）。
- 写入：`INSERT … ON DUPLICATE KEY UPDATE stats, markdown, created_at`。MySQL 不可用时记日志，下一个整点重试一次（最多 3 次）。
- 接口：`GET /api/ai/reports?limit=30` → `[{day, created_at, llm, nodes, tasks}]`；`GET /api/ai/reports/{day}` → 整行（stats、markdown）。

### 7. 看板

- 问答页 `Chat.vue`：消息列表（用户/助手）、输入框、发送中禁用；助手消息下方折叠显示工具调用（名称、参数、结果概括、耗时）；用 `fetch` + `ReadableStream` 解析 SSE（`EventSource` 不支持 POST）；`error` 事件显示原因（未启用 LLM 时提示去设置页）。历史只保留在页面内存（刷新清空）。
- 日报页 `Reports.vue`：左侧日期列表（`GET /api/ai/reports`），右侧 Markdown 正文（`marked` 渲染，关闭 HTML 直通）+ 数字表格（按 `stats` 渲染，正文被否决时只显示表格并注明原因）；“重新生成”按钮调 POST。
- 总览卡片新增一行“预测 30 秒 CPU”：Manager 节点 JSON 有 `pred_cpu` 时显示它与 `f_pred`（< 1 时标橙）；推送关闭时每 5 秒读 `/api/ai/predictions`，显示 AI 算出的值并注明“未参与调度”；节点详情分数明细里加 `f_pred`。
- 设置页加一张“负载预测”卡：启用/推送状态、1 小时 MAE（Holt 与基线）、点数。
- 导航：总览、事件、诊断、问答、日报、设置。

### 8. 部署与联调

- Nginx 新增 `location = /api/ai/chat { proxy_pass …; proxy_buffering off; proxy_cache off; proxy_read_timeout 300s; proxy_set_header Connection ""; proxy_http_version 1.1; }`，其余 `/api/ai/` 不变（60 秒）。
- compose `ai` 加 `LM_MANAGER_ADDR: 127.0.0.1:50051`；`.env.example` 不加新项。
- 镜像：`pip install grpcio grpcio-tools mcp apscheduler`；构建时 `python -m grpc_tools.protoc -I proto --python_out --grpc_python_out ai/lmai/pb proto/advisor.proto proto/monitor.proto`（生成文件不进 Git，`ai/lmai/pb/__init__.py` 进 Git；本机测试时由 `conftest.py` 在缺失时生成一次）。
- `mock_llm.py`：请求含 `tools` 且对话里还没有 `tool` 消息时返回一次 `list_nodes` 的 tool_call，否则返回文本 `[mock] …`（引用工具结果里的节点数）；`stream: true` 时按 SSE 分块输出（`data: {...}` + `data: [DONE]`），带 usage；新增 `report` 模式（返回一段包含 stats 里数字的 Markdown）和 `report_bad` 模式（故意写错一个数字，测核对）。

## Risks / Trade-offs

- [模型用工具不当或编造] → 系统提示词要求引用工具返回的数值；`chat_eval.py` 用 20 个可自动判分的问题测正确率；工具结果里带单位和时间，减少换算错误。
- [SSE 经 Nginx 被缓冲] → 专门的 `location = /api/ai/chat` 关缓冲；验收时用 `curl -N` 看首字节时间。
- [Holt 在负载突变时过冲（趋势项外推 30 秒）] → 预测值限幅 [0, 1]、因子只降不升且不低于 0.5；在线和离线都和基线比 MAE，比不过就关 `predict.push`。
- [预测因子让一台“看起来要忙”的节点少接任务，形成自我实现] → 因子最低 0.5，不会清零；10 秒过期；评估时记录任务占比变化。
- [日报数字核对过严（LLM 换个写法就被否决）] → 允许集合覆盖常见写法（取整、1–2 位小数、百分数、ms↔s）；提示词禁止模型自己算；被否决时仍有完整表格，不丢信息。
- [`score_weights` 与 `manager.yaml` 不一致] → 配置注释说明；`explain_score` 的输出里同时给出 Hash 里的 `score` 和按权重重算的值，不一致时标注“权重配置可能与 Manager 不同”。
- [AI 进程内多了 gRPC 客户端和 APScheduler] → 都在独立 asyncio 任务里，失败只记日志；`stop()` 时取消。
- [`grpcio` 在 `python:3.11-slim` 上要编译] → 用官方 wheel（aarch64 有预编译），构建期拉包与 M8 相同。

## Migration Plan

1. Mac 上构建 `lm-manager`、`lm-ai`、`lm-web`，先打回滚标签（`lm-manager:m7`、`lm-ai:m8`、`lm-web:m8` 已有）。
2. 先更新 Manager（新服务注册、`f_pred` 字段），此时没有预测推来，`f_pred = 1`，行为与 M7 一致。
3. 再更新 AI 服务（`predict.push` 先开，观察 `/api/ai/predictions` 的 MAE；比不过基线就关）与看板。
4. 回滚：`docker compose up -d --no-build` 换回旧标签；Redis 里多出的 `ai:pred`、`f_pred` 字段无害。

## Open Questions

- 用户是否要给 MCP server 配到自己的 Claude Code（需要本机能连虚拟机的 Redis/MySQL 端口，或在虚拟机上 `docker run -i`）。不影响实现，验收时给出两种接法。

## 实现中的决定

- **阻尼趋势 Holt（φ = 0.9，α = 0.3，β = 0.05），`predict.push` 默认关闭**：原定普通 Holt（α 0.3、β 0.1）。虚拟机 11-15 23:00–24:00 的真实数据（lm-worker-1 + lmtest-worker2，1433 个对比点）上普通 Holt 的 MAE 0.0871，比朴素基线 0.0649 高 34%：趋势项把一时的升降线性外推 30 秒，CPU 毛刺多时过冲。扫参后改成阻尼趋势（Gardner–McKenzie，仍是水平 + 趋势两个量）：这一小时 MAE 0.0630，比基线低 1.7%；合成缓升负载上低约 4.5%。φ 0.8 在真实数据上更好但在干净的线性爬升上不如基线，所以取 0.9。
  但再细看：只算因子可能起作用的高负载段（实际值、基线或预测 ≥ 0.5，114 个点），Holt 0.3420 对基线 0.3417，持平；凌晨空闲的一小时（03:12–04:12，在线统计）Holt 反而高 12–19%（绝对值只差约 0.002）。这两台节点上的高负载都是突然出现的，30 秒前看不出趋势。缓升负载注入时 `f_pred` 在 CPU 越过 0.7 后 4 秒才开始下降，没有提前量，只是在 0.7–0.97 之间额外压低 8%–37% 的权重。按开发文档“比不过朴素基线就关掉”，`predict.push` 默认 false：预测、`ai:pred` 和在线 MAE 照常计算，看板设置页可看；Manager 侧的 Advisor 服务与 `f_pred` 保留，打开推送即可生效（停 AI 后 6 秒回到 1 已验证）。
- **Holt 用标准库实现**：每台节点每秒几次浮点运算，numpy 反而要来回转换；离线评估脚本因此能在本机 Python 3.9 直接复用。
- **预测也用 `node:{id}:ts` 预热**：AI 服务重启后不用等 30 秒样本才有预测。
- **超时拆成执行超时与确认超时**：`task` 表的 `timeout` 同时包含“分配后 30 秒 Worker 没确认收到”和“收到后 300 秒没完成”。11-15 的 4038 个超时全部是前者（16 点那一小时，M8 验收时 token 带引号导致 Worker 拒收），日报和 `get_task_stats` 现在分开给出 `exec_timeouts`、`unconfirmed`。
- **日报提示词要求整数原样引用**：第一次用 DeepSeek 生成 11-14 的日报时，模型把 P99 310002 ms 写成“310000”，被核对否决。核对保持严格（验收要求 100% 一致），改提示词：整数原样写，毫秒可以换算成秒。被否决的正文存进 `stats.llm_text` 便于排查。改后 11-14 的日报通过核对。
- **`ai_daily_report` 写入用 `INSERT … AS new`**：MySQL 8.0.20 起 `VALUES()` 弃用并告警。
- **MCP server 需要挂 `/etc/localtime`**：和 compose 里的 `ai` 一样，否则工具输出的时间是 UTC。README 里的命令已带上。虚拟机上为 MCP 和评估脚本写了 `~/lm/deploy/.env.mcp`（600 权限，从 `.env` 派生 Redis/MySQL 的连接变量）。
- **部署只换镜像**：AI 默认把预测推到 `127.0.0.1:50051`，`ai` 是 host 网络，虚拟机上的 compose 文件不用改；仓库里的 compose 加了 `LM_MANAGER_ADDR` 只是写明。
- **评估脚本标准答案不经过问答工具**：`chat_eval.py` 直接查 Redis/MySQL，提问前后各取一次，实时指标与其中任一个在容差内算对。
- **verify 后的修正（11-16）**：代码对、文档错的改文档，文档对、代码错的改代码。
  - 改代码：总览在推送关闭时改读 `/api/ai/predictions`，显示 AI 算出的预测并注明“未参与调度”（原来总是“—”）；`/api/ai/health` 的 `report` 按 spec 增加 `last_day`；`Anonymizer` 加公开的 `hide_text()`，`chat.py` 不再调私有方法。
  - 改文档：`ai:pred` 在 `predict.enabled` 时就写，推送另看 `predict.push`；日报的触发指标计数来自当天诊断；本文档的日报统计示例换成实际字段；Advisor 对非有限值按“无预测”处理而不是拒绝整批，并补进 `manager-allocation` 的 Advisor 需求。
  - 归档前把 `manager-allocation` 改成对「候选集合与权重」的 MODIFIED（m5b、m6 同样处理），去掉“自本变更起”的叠加写法。
