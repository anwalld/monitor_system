# Design

## 进程结构

一个 Python 进程（uvicorn + FastAPI），在 lifespan 里起四个 asyncio 任务：

| 任务 | 做什么 |
|---|---|
| metrics 消费 | `XREADGROUP ai stream:metrics`，每条（所有节点一秒的样本）喂给检测器，`XACK` |
| events 消费 | `XREADGROUP ai stream:events`，`evict`、`rescue_start` 直接生成异常；记下每台节点最近的状态转移；忽略自己写的 `diagnosis` |
| 诊断 | 从队列（上限 32，满了丢最旧的）取异常，组装上下文、调 LLM、写 MySQL 和 `stream:events` |
| MySQL 补写 | 写 `ai_diagnosis` 失败的行放进内存缓冲（上限 200），每 5 秒重试 |

检测在消费任务里同步完成（20 台 × 约 12 个指标 × 600 点的中位数，每秒不到 10 ms），不等 LLM。LLM 调用在诊断任务里 `await`，卡住只影响诊断。

代码在 `ai/lmai/`：`config`（YAML + 环境变量）、`windows`（滑动窗口与稳健 z 分数）、`detector`（规则、去重）、`context`（组装上下文、匿名化）、`llm`（厂商选择、调用、重试、预算）、`schema`（pydantic 模型）、`diagnosis`（流程编排）、`store`（Redis、MySQL）、`api`（FastAPI 路由）、`main`。

## 检测

- **指标**：`cpu`、`runq_p99_us`、`mem_avail`、`disk`，每块网卡 `nic.<名>.rtt_ms`、`nic.<名>.timeout_rate`、`nic.<名>.retrans_rate`。
- **方向**：只看“变坏”的方向——可用内存看下降，其他看上升。负载结束时 CPU 骤降不算异常。
- **z 分数**：用当前点之前的窗口算中位数和 MAD（当前点不参与）；窗口按时间保留 10 分钟。MAD 的下限是每个指标的最小尺度（cpu 0.05、runq 100 µs、mem 0.02、disk 0.05、rtt 2 ms、超时率 0.05、重传率 0.01），写在 `ai.yaml`。这样常量为 0 的超时率不会因为一次超时就报，CPU 至少要高出中位数 4 × 1.4826 × 0.05 ≈ 0.30。
- **连续 3 点**：每个指标一个计数器，连续 3 个点越界在第 3 个点报一次；之后同一段持续越界不再重复报，回到界内才清零。
- **窗口少于 60 个样本**（新节点、刚恢复）不做 z 分数检测。
- **阈值规则**：排队延迟丢弃率 > 1%、网卡超时率 > 20% 用最近 10 秒的平均值判断，也要连续 3 个点。虚拟机 ens160 到 8.8.8.8 经 NAT 本身丢约 11%，每秒的探测次数少，单秒的超时率只能取 0、0.5、1 这类值，直接按单点判断会频繁误报。
- **事件规则**：`evict`、`rescue_start` 直接生成异常；只处理最近 10 分钟内的事件（重启后积压的旧事件只确认不处理）。
- **严重度**（检测器给的）：`evict`、`rescue_start` 为 high；阈值规则为 medium；z 分数 |z| ≥ 10 为 high，否则 medium。LLM 合格时诊断的最终严重度用 LLM 给的，否则用检测器的。
- **去重**：每台节点记上一次诊断的时间和检测器严重度；10 分钟内只有严重度更高才再诊断。去重在入队前做，被去重的异常仍记进最近异常列表（`/api/ai/anomalies`，评估用）。
- **合并**：诊断任务取出一个异常后，等到检出后 5 秒（`settle_s`）再组装上下文，把该节点检出前 60 秒到检出后 5 秒内的其他异常一起放进去。虚拟机上 CPU 故障时调度延迟先在第 4 秒报、CPU 第 5 秒、剔除第 6 秒，不等的话剔除进不了上下文。

## 诊断上下文

```json
{
  "anomaly": {"node": "...", "rule": "zscore|threshold|event", "metric": "cpu", "value": 0.98, "median": 0.03, "z": 12.4, "severity": "high", "ts": "..."},
  "related_anomalies": [...],
  "node_state": {"state": "normal", "score": 0.41, "s_cpu": ..., "f_timeout": ..., "timeout_count": ..., "evicted": "0", "inflight_tasks": ...},
  "recent_metrics": {"step_s": 10, "ts": [...], "cpu": [...], "runq_p99_us": [...], ...},
  "state_transitions": [{"ts": "...", "kind": "evict", "detail": {...}}],
  "task_timeouts": {"count_30m": 2, "latest": ["..."]},
  "peer_median": {"nodes": 3, "cpu": 0.12, ...}
}
```

- `recent_metrics` 来自检测窗口，按 10 秒分桶取平均（runq 取最大），最多 60 个点。
- `node_state` 读 `node:{id}` Hash（只读），只取数字和状态字段，不含地址。
- `task_timeouts` 查 MySQL `task` 表最近 30 分钟 `state='timeout'`；MySQL 不可用时写 `null`。
- 系统提示词说明每个指标的含义、取值范围和调度阈值，要求 evidence 引用上下文里的具体数值，只用中文，只输出 JSON。
- 匿名：`anonymize: true` 时节点名按字典序换成 `node-1`、`node-2`……，返回后把文本里的代号换回。

## LLM

- `ai.yaml` 的 `vendors`：每个厂商 `label`、`base_url`、`models`、`key_env`（`.env` 里的变量名）、`json_mode`（是否支持 `response_format={"type":"json_object"}`）。预置 DeepSeek（`https://api.deepseek.com`，deepseek-chat、deepseek-reasoner）和通义千问（`https://dashscope.aliyuncs.com/compatible-mode/v1`，qwen-plus、qwen-max、qwen-turbo）。`mock` 厂商只在测试配置里出现。
- 每次诊断开始时读一次 `ai:settings`（vendor、model），换设置不用重启。
- `AsyncOpenAI(timeout=30, max_retries=0)`，自己做重试：超时、连接错误、429、5xx 等 1 秒、2 秒后再试，最多重试 2 次。
- 校验失败（JSON 解析或 pydantic）时把错误追加进对话重试一次，这次重试不计入上面 2 次。
- token：每次响应的 `usage.total_tokens` 加到 `ai:tokens:{YYYYMMDD}`（2 天过期）。调用前已达到 `daily_token_budget`（默认 1000000；原定 200000，用户 2026-11-15 调高）就不调用。
- 不调用 LLM 时的统计诊断：`summary` 由代码生成（如“CPU 忙碌度 0.98，比最近 10 分钟中位数 0.03 高 12.4 个尺度”），`causes`、`actions` 为空，`report.llm` 写明原因（`disabled`、`no_key`、`budget`、`error: ...`、`invalid`）。

## 输出 schema

```python
class Cause(BaseModel):
    cause: str
    evidence: list[str]          # 至少 1 条
    confidence: float            # 0~1

class Report(BaseModel):
    severity: Literal["low", "medium", "high"]
    summary: str                 # 1~200 字
    causes: list[Cause]          # 1~5 条
    actions: list[str]           # 0~5 条
```

`extra="forbid"`。写进 `ai_diagnosis.report` 时额外加 `llm`（`ok` 或不调用的原因）和 `attempts`。

## 写入

- `ai_diagnosis`：`anomaly` 存触发异常和相关异常，`report` 存上面的 JSON，`model` 为 `厂商/模型`（统计诊断为 NULL），`tokens` 为本次总 token。
- 插入成功拿到自增 id 后 `XADD stream:events MAXLEN ~ 10000 * node <id> kind diagnosis ts_ms <ms> detail <json>`，detail：`{"diag_id", "severity", "summary", "metric", "rule", "model"}`。MySQL 不可用时先发事件（`diag_id` 为 null），行进补写缓冲。
- Redis key 支持前缀（`LM_AI_KEY_PREFIX`，测试用）。

## 接口

| 路径 | 说明 |
|---|---|
| `GET /api/ai/health` | 不鉴权：redis、mysql、llm（vendor/model/enabled/原因）、tokens_today、budget、nodes、queue、anomalies_total、diagnoses_total |
| `GET /api/ai/diagnoses` | node、severity、hours（默认 24）、limit（默认 100，最大 1000） |
| `GET /api/ai/diagnoses/{id}` | 完整一行 |
| `GET /api/ai/anomalies` | 内存里最近 1000 个检测器异常（含被去重的），评估用；参数 since_ms |
| `GET`/`PUT /api/ai/settings` | 见 spec |

鉴权与 Manager 相同：`API_TOKEN` 为空不鉴权。

## 部署

- `deploy/docker/ai.Dockerfile`：`python:3.11-slim`，`pip install -r ai/requirements.txt`，复制 `ai/` 和 `config/ai.yaml`。
- compose `ai` 服务：host 网络，环境变量 `LM_REDIS_URI`、`LM_REDIS_PASSWORD`、`LM_MYSQL_*`、`LM_API_TOKEN`、各厂商 Key（`DEEPSEEK_API_KEY`、`DASHSCOPE_API_KEY`），`depends_on: redis`。
- nginx：`location /api/ai/` 放在 `/api/` 前面（最长前缀匹配，顺序其实无关），`proxy_pass http://${AI_HTTP}`，`proxy_read_timeout 60s`（M9 的 SSE 再调）。

## 实现中的决定

- **基础镜像** `python:3.11-slim`（开发文档定的 3.11）。虚拟机拉不到 Docker Hub，和其他镜像一样在 Mac 上构建后 `docker save | ssh docker load`。镜像里带 pytest 和测试，`docker run --rm lm-ai python -m pytest -q` 直接跑。
- **超时套两层**：SDK 的 `timeout=30` 只管单次 HTTP 读写，外面再套 `asyncio.wait_for(30 + min(5, 30))`，防止流式读写一直有零星数据时整体超过 30 秒太多。
- **BYOK**（用户要求，2026-11-15）：开发文档原定“Key 放 .env，不经过前端”，改为设置页可以直接填 Key。PUT 时写进 Redis `ai:keys`（按厂商的 Hash），GET 只返回来源（ui/env）和末 4 位；看板填的优先于 `.env`，可删除后回落到 `.env`。代价：Key 明文存在 Redis（Redis 有密码、只在本机端口映射）；看板走 HTTP 时 Key 在局域网明文传输；`API_TOKEN` 为空时任何能打开看板的人都能改 Key（但读不到）。正式使用建议设 `API_TOKEN`。
- **去掉 deepseek-reasoner**：不确定它是否支持 `response_format=json_object`，不支持时每次都会 400。只保留 deepseek-chat，以后要加推理模型再按模型配 `json_mode`。
- **MySQL 不可用时**：诊断事件照常发（`diag_id` 为 null），行进内存缓冲（上限 200），每 5 秒补写。本机暂停测试 MySQL 验证：事件立即出现，恢复后 33 秒内补写成功。查任务超时也走 MySQL，断开时这一项写 null，会让诊断多等约 8 秒（连接超时）。
- **完整上下文存进 `ai_diagnosis.anomaly.context`**（约 3 KB/行），看板诊断详情可以展开查看“发给模型的上下文”，便于人工判断模型是否引用了真实数值。
- **虚拟机验收先用测试容器**：`lmtest-ai`、`lmtest-mockllm`、`lmtest-nginx`（8088 端口，新看板镜像 `lm-web:m8`），不动 compose 项目 `lm`（改 compose 要先问用户）。AI 服务连的是正式的 Redis 和 MySQL，所以 `ai_diagnosis` 和 `stream:events` 里会留下测试期间的诊断，用 mock 模型的诊断结论以 `[mock]` 开头。验收后经用户同意（16:57，由另一个会话操作）并入 compose：`ai` 服务 + nginx 换 `lm-web:m8`，测试容器已删；回滚用 `docker-compose.yml.m7.bak` 和 `lm-web:m7`。
- **归档顺序**：本变更对 `web-dashboard` 用了 MODIFIED（Nginx 入口），要先归档 m7b 再归档本变更。
