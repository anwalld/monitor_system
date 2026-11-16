# Tasks

## 1. Manager：Advisor 服务与 f_pred

- [x] 1.1 `NodeTable`：`NodeState` 增加 `pred_factor`、`pred_cpu`、`pred_until_mono_ms`；`set_predictions()` 一次加锁赋值（限幅、未知节点计数）；`factors_for` 的 `f.pred` 按过期判断；`AcceptedSample`/`NodeView` 增加 `f_pred`、`pred_cpu`；`manager.yaml` 新增 `advisor` 段（`enabled`、`min_factor`、`max_ttl_ms`）。验证：`test_node_table` 新增用例——推送后概率变化、过期后回到 1、越界限幅、未知节点忽略
- [x] 1.2 `manager/api/advisor_service.{h,cpp}`：实现 `Advisor.PushPredictions`，`main.cpp` 注册；`redis_writer` 写 `f_pred`、`pred_cpu`（过期 HDEL）；`node_json` 增加两个字段。验证：`test_manager_rpc` 新增进程内 gRPC 用例（推送后 `get()` 里有因子、ttl 为 0 用默认、`enabled=false` 不记录）；`test_web_json`、`test_redis_writer` 覆盖新字段；本机 `ctest` 全过
- [x] 1.3 【Linux】`lm-builder` 容器里构建并跑 `ctest`（含 Redis/MySQL 集成测试）；构建 `lm-manager` 镜像。验证：全部通过，镜像可启动
  - 结果：`lm-builder` 里 229 个测试全过（含 Redis/MySQL 集成测试，2 个跳过是需要 root/Linux 宿主机的用例）；本机 macOS 243 个全过。`lm-manager` 镜像构建并部署

## 2. AI 服务：工具集与问答

- [x] 2.1 `ai/lmai/tools.py`：六个只读工具 + JSON Schema 描述；`store.py` 增加所需查询（节点 Hash 列表、5 分钟聚合、node_event、task 统计、ai:pred）；`config/ai.yaml` 增加 `chat` 段（`max_tool_calls: 6`、`timeout_s: 120`、`history_max: 10`、`score_weights`）。验证：pytest 用假 Redis/MySQL 覆盖每个工具的正常返回、未知节点、参数限幅、降采样点数 ≤ 60
  - 结果：pytest 10 个用例；对着真实测试 Redis/MySQL 逐个调用六个工具均正常（5 分钟聚合、node_event、task 的 TIMESTAMPDIFF 都走真 SQL）
- [x] 2.2 `ai/lmai/llm.py`：增加流式工具调用 `stream_chat(messages, tools)`（tool_calls 增量拼接、usage 统计、预算检查、`tool_choice`）；`ai/lmai/chat.py`：问答编排（工具循环上限 6、总超时、SSE 事件生成、匿名化往返）；`api.py` 增加 `POST /api/ai/chat`。验证：pytest 用假客户端脚本（先 tool_calls 再正文）检查事件顺序、6 次上限后强制成文、未启用返回 error、token 计入预算
  - 结果：pytest 5 个用例（事件顺序、上限后 `tool_choice=none`、未启用/超预算、历史清洗、SSE 接口与鉴权）
- [x] 2.3 `ai/chat/mcp_server.py`（FastMCP、stdio），`requirements.txt` 加 `mcp`。验证：用 `mcp` 客户端（stdio）在容器里列出 6 个工具并调用 `list_nodes`
  - 结果：容器里 stdio 客户端列出 6 个工具；虚拟机上接正式 Redis/MySQL 调用 `list_nodes` 返回 2 台节点（需挂 `/etc/localtime`，否则时间是 UTC）
- [x] 2.4 `experiments/mock_llm.py`：函数调用（先返回 `list_nodes` 的 tool_call，再返回正文）、`stream: true` 分块输出、`report`/`report_bad` 模式。验证：本机 AI 服务 + mock + `lmtest-redis`/`lmtest-mysql`，`curl -N /api/ai/chat` 看到 tool_call → tool_result → delta → done
  - 结果：`curl -N` 依次收到 tool_call → tool_result → 7 条 delta → done

## 3. AI 服务：负载预测

- [x] 3.1 `ai/lmai/predict.py`：Holt 状态与更新、预测限幅、因子换算、在线 MAE（Holt 与基线，1 小时窗口）；`config/ai.yaml` 增加 `predict` 段（`enabled`、`push`、`interval_s`、`horizon_s`、`alpha`、`beta`、`min_samples`、`ttl_ms`、`manager_addr`，环境变量 `LM_MANAGER_ADDR` 覆盖）。验证：pytest——线性爬升序列的 Holt 误差小于基线、常量序列预测等于常量、因子换算表（0.5/0.7/0.85/1.0/1.2）、MAE 累计与窗口修剪
  - 结果：pytest 4 个用例（缓升序列 Holt MAE < 基线、常量序列 MAE 为 0、乱序忽略、窗口修剪）。Holt 用标准库实现（量很小，numpy 反而要转换），离线评估脚本可在本机 Python 3.9 复用
- [x] 3.2 gRPC 推送：`ai/lmai/pb/`（构建时 `grpc_tools.protoc` 生成，`conftest.py` 缺失时生成）、推送任务（每 5 秒、超时 2 秒、失败限频日志）、写 `ai:pred` 并过期；`api.py` 增加 `GET /api/ai/predictions`，health 加 `predict`；`diagnosis.py` 的样本回调接入 `Predictor.observe`。验证：pytest 用假 stub 检查批内容（ttl 10000、只含新鲜节点）；本机 AI 服务对着本机 Manager（18080/50051）推送，Manager 日志与 `node:{id}` Hash 出现 `f_pred`
  - 结果：pytest 含真实 gRPC 往返（Python 端起 Advisor 服务）；虚拟机上 Manager 日志“预测批 N（未知节点 0）”，`node:lm-worker-1` 有 `f_pred`、`pred_cpu`，`ai:pred` TTL 10 秒。另加：启动预热时用 `node:{id}:ts` 喂预测器，重启后立即有预测
- [x] 3.3 `experiments/predict_eval.py`：读 `node:{id}:ts` 最近 1 小时，离线回放 Holt（可扫 α、β）与基线的 MAE。验证：对合成数据（`replay_metrics.py`）和虚拟机真实数据各输出一张对比表
  - 结果：合成数据（`replay_metrics.py`，平稳加噪声）和虚拟机真实数据各一张表；`--grid` 扫 α × β × φ。数据见 6.3

## 4. AI 服务：日报

- [x] 4.1 `ai/lmai/report.py`：`compute_stats(day)`（可用时长、剔除/急救、任务与 P50/P99、诊断、异常计数；本地日界 → UTC 区间）、`render_table(stats)`、数字提取与核对、`generate(day)`；`store.py` 增加对应查询与 `ai_daily_report` 写读；`config/ai.yaml` 增加 `report` 段（`at: "09:00"`、`enabled`）。验证：pytest——构造 stats 验证表格；核对：全部一致通过、多一个数否决、日期/时间不算数字、百分数与 ms↔s 换算被接受；日界换算
  - 结果：pytest 4 个用例；测试 MySQL 上本地日界正确（前一天 23:59 的任务、次日 00:00:01 的剔除都不算），`TIMESTAMPDIFF` 得到 200/500/100 ms
- [x] 4.2 APScheduler 定时（`report.at` 生成昨天、启动补生成）、`api.py` 增加 `GET /api/ai/reports`、`GET/POST /api/ai/reports/{day}`，health 加 `report`；`requirements.txt` 加 `apscheduler`。验证：本机 AI 服务 + mock（`report` 模式）`POST /api/ai/reports/<昨天>` 生成成功且 `llm = ok`；mock `report_bad` 模式下 `llm = mismatch` 且 markdown 为表格
  - 结果：mock `report` 模式 `llm = ok`；`report_bad` 模式 `llm = mismatch`、`mismatched = ['12.345']`、正文换成表格；不存在的日期 404、格式错 400。改用 MySQL 8 的 `INSERT … AS new` 写法消除 `VALUES()` 弃用警告

## 5. 看板与部署

- [x] 5.1 看板：问答页 `Chat.vue`（SSE 解析、工具条目、错误提示）、日报页 `Reports.vue`（`marked`，日期列表、表格、重新生成）、总览预测行、节点详情 `f_pred`、设置页预测卡、导航。验证：`npm run build`；内置浏览器对着本机服务：提问后工具条目和逐字回答、日报页显示、总览显示预测
  - 结果：构建 660 KB（gzip 227 KB，多了 marked）；内置浏览器对着本机 AI 服务：问答页工具条目（可展开）和流式回答、日报页列表/正文/表格正常
- [x] 5.2 Nginx `location = /api/ai/chat`（不缓冲、300 秒）、compose `LM_MANAGER_ADDR`、`ai.Dockerfile`（grpcio-tools 生成 pb、新依赖）、`deploy/README`、`README`。验证：`lm-ai` 镜像内 pytest 全过；`lm-web` 镜像构建；本机 `docker run lm-web` 经 Nginx `curl -N` 问答首个事件 2 秒内到达
  - 结果：镜像内 pytest 53 个全过；本机 `lm-web` 经 Nginx 问答事件按 mock 的 20 ms 节奏逐条到达（tool_call 28 ms、done 204 ms），没有被缓冲

## 6. 验收

- [x] 6.1 【Linux】部署 Manager、AI、看板到虚拟机 compose 项目 `lm`（先打回滚标签）；确认 `f_pred` 出现在 Hash 和看板，`ai:pred` 每 5 秒刷新，Worker 上报与任务分配不受影响
  - 结果：11-15 23:46 换镜像（回滚标签 `lm-manager:m7`、`lm-ai:m8`、`lm-web:m8`），compose 文件未改。两台节点重新登记、慢启动；Manager 日志“预测批 N（未知节点 0）”；`node:lm-worker-1` 有 `f_pred`、`pred_cpu`，`ai:pred` TTL 10 秒；随后 lm-bench 5 个/秒 × 180 秒：882 个任务全部完成，两节点各约一半，P99 227 ms。之后 AI 又换了 2 次镜像（日报提示词、阻尼 Holt）
- [x] 6.2 【Linux】问答验收：`experiments/chat_eval.py` 对真实 LLM（DeepSeek）跑 20 题，记录正确率、平均工具调用次数、平均耗时；MCP server 在容器里用 stdio 客户端列出工具并调用。验收：正确率达标（≥ 80%，开发文档未给具体值，取此为门槛并记录）
  - 结果（DeepSeek deepseek-chat，经 Nginx 80 端口，11-15 23:52）：**正确 20/20 = 100%**，平均工具调用 1.00 次，平均耗时 2.3 秒，首个 SSE 事件中位 0.79 秒（最大 1.09 秒），每题约 3700 token（20 题共 8.1 万）。题目覆盖节点数/在线数、最闲/最低分节点、实时 CPU、状态、f_timeout、分项分、best_nic、24 小时剔除次数、最近诊断严重度、1 小时任务数/P99/超时、10 分钟 CPU 平均/P99 最大/内存最低、f_pred、最近剔除原因、当前剔除节点。MCP：虚拟机上 `docker run -i … python -m chat.mcp_server` + stdio 客户端列出 6 个工具，`list_nodes` 返回两台节点的真实数据。小问题：模型调用工具前说的“我先查一下……”会拼在回答开头
- [x] 6.3 【Linux】预测验收：`predict_eval.py` 用虚拟机和测试节点最近 1 小时数据对比 MAE；在线 `GET /api/ai/predictions` 观察 ≥ 30 分钟；对测试节点注入缓升负载（`stress` 逐步加线程），记录 `f_pred` 下降与任务占比变化。验收：Holt 的 MAE 不高于基线，否则关闭 `predict.push` 并记录
  - 结果（详见 design“实现中的决定”）：普通 Holt 在虚拟机忙时一小时 MAE 0.0871，基线 0.0649（差 34%）→ 改阻尼趋势后 0.0630 对 0.0641（好 1.7%），但高负载段（≥ 0.5，114 点）0.3420 对 0.3417 持平；在线 4 小时后凌晨空闲一小时 0.0131 对 0.0110（差 19%）。缓升注入（测试节点每 15 秒加一个 nice 19 的 stress -c 1，lm-bench 5 个/秒）：CPU 93 秒越过 0.7、`f_pred` 97 秒才降（没有提前量），100–140 秒 `f_pred` 0.92 → 0.63，测试节点任务占比 0.53 → 0.48（lm-worker-1 同时约 0.6 CPU），140 秒剔除后为 0；1484 个任务 0 失败。**按开发文档“比不过就关掉”，`predict.push` 默认改为 false**，预测与在线评估保留
- [x] 6.4 【Linux】日报验收：`POST /api/ai/reports/<昨天>` 用 DeepSeek 生成，人工核对正文数字；连续生成 ≥ 3 天（含定时触发一次）统计数字一致率。验收：正文与 stats 一致率 100%（被否决的算正文不一致，但表格仍正确）
  - 结果（DeepSeek）：11-14 第一次被核对否决（P99 310002 ms 被写成“310000”），改提示词后 11-14、11-13（无数据）、11-15 共 5 次生成全部通过核对；11-15 的统计与独立手写 SQL 逐项一致（可用秒数、剔除次数、各状态任务数、诊断分级），正文 30 多个数字人工逐个核对无误。定时触发：本机测试容器把 `report.at` 设为 2 分钟后，00:03:00 准时生成前一天的日报，下一次排到次日；虚拟机上 11-16 09:00:03 定时生成 11-15 的日报（DeepSeek，`llm = ok`，通过核对）
- [x] 6.5 【Linux】故障隔离：停 AI 服务 60 秒（压测中）——任务 0 失败、`f_pred` 10 秒内回到 1；AI 连不上 Manager（iptables 丢 50051 回环包 60 秒，自动撤销）——AI 只记日志，问答与诊断照常；SSE 经 Nginx 不缓冲（`curl -N` 首事件时间）
  - 结果：停 AI 60 秒（lm-bench 5 个/秒 × 180 秒，测试节点 8 个 stress 使 `f_pred` ≈ 0.8）：882 个任务 0 失败，看板接口 0 错误，停后 6 秒 `f_pred` 回到 1、选中概率 0.28 → 0.32，重启后 8 秒预测恢复（Redis 预热）。iptables 只丢 AI 进程（uid 10001）发往回环 50051 的包 58 秒（Worker 上报不受影响，systemd-run 90 秒自动撤销）：推送失败 8 次只记日志（14 秒一条），检测样本照常增长，期间问答正常完成，撤销后 12 秒推送恢复。SSE 经 Nginx：首事件中位 0.79 秒，mock 下事件按 20 ms 节奏逐条到达
- [x] 6.6 更新 README、deploy/README、HANDOFF（含 MCP 接法、评估数据、开关建议）
  - 结果：README（M9 小节、接口、MCP 接法）、deploy/README（第 8 节）、HANDOFF 已更新

## Workflow follow-up

- 归档顺序：本变更对 `web-dashboard`（Nginx 入口）用了 MODIFIED，并对 `manager-allocation` 用 ADDED 覆盖了 m5b 的“f_pred 仍取 1”，要先归档 m5a → m5b → m6 → m7b → m8，再归档本变更，并把本变更的 `manager-allocation` 条目改成 MODIFIED（见 HANDOFF“归档时要改的 spec 叠加问题”）。
