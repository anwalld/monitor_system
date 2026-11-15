# Proposal

## Why

对应开发文档里程碑 M7 的后半部分。m7a 之后事件进了 `stream:events`、冷数据进了 MySQL，但还没有任何读接口和界面：

- 开发文档的看板（总览、节点详情、事件页）要从 Manager 的 REST 和 WebSocket 读数据。
- M7 的验收“看板断线重连后事件不丢不重”需要 `?since=` 补发和前端按 id 去重。
- Nginx 是 Manager 侧唯一入口（`/api/`、`/ws` → 8080，`/` → 静态文件）。

## What Changes

- **Manager 对外接口**（`manager/api/web_*`）：Drogon 在 Manager 进程内另起线程监听 8080（和 gRPC 各用各的线程）。
  - REST：`GET /api/health`、`/api/nodes`、`/api/nodes/{id}`、`/api/nodes/{id}/series`（最近 1 小时降采样到每 5 秒一个点，共 720 点）、`/api/nodes/{id}/events`、`/api/events`。
  - WebSocket `/ws`：连上先收 `snapshot`，之后每秒一条 `node_update`，事件实时推 `event`；`?since=<id>` 时先用 `XRANGE` 补发再发 `snapshot`。
  - 一个线程阻塞 `XREAD stream:events`，把所有事件（以后包括 AI 写入的诊断）推给所有连接。
  - 静态 token：请求头 `Authorization: Bearer <token>` 或查询参数 `token=`；配置 `api.token`，环境变量 `LM_API_TOKEN` 覆盖；为空时不鉴权（启动时警告）。
  - 找不到 Drogon 时接口编为桩（只记一条警告），其余功能不受影响。
- **看板**（`web/`）：Vue 3 + Vite + ECharts 单页应用：总览（节点卡片）、节点详情（曲线、分数明细、事件时间线）、事件页（按节点、种类筛选）。AI 相关页面（问答、日报、设置、诊断）留到 M8/M9。断线后按 1、2、4…30 秒退避重连，带 `since`，按事件 id 去重。
- **Nginx**：`deploy/nginx/nginx.conf`；`deploy/docker/web.Dockerfile` 多阶段构建（node 构建 → nginx 托管）；compose 增加 `nginx` 服务（host 网络，端口 `HTTP_PORT`，默认 80）。
- **验收工具**：`experiments/ws_check.mjs`（Node 22 自带 WebSocket）：断线、等待事件、带 since 重连，与 `/api/events` 对比是否不丢不重。

## Capabilities

### New Capabilities
- `manager-web-api`：REST 与 WebSocket 接口、消息格式、补发与鉴权。
- `web-dashboard`：看板页面、重连与去重、Nginx 路由。

## Impact

- 对应里程碑：M7（后半）。影响模块：manager（`api/` 新增 Web 接口，`main.cpp` 启动），web（新增），deploy（nginx、web 镜像、compose、Manager 镜像加 Drogon 运行库），config，experiments，tests。
- proto 不改；Redis、MySQL 结构不改，只读。
- 新依赖：Drogon（Ubuntu `libdrogon-dev` 1.8.7，macOS Homebrew 1.9.13），前端 npm 依赖（vue、vue-router、echarts、vite）。
- 调度锁：`node_update` 每秒取一次节点表快照（≤ 20 台，拷贝出来后锁外序列化）。
- 不在范围内：AI 页面与 `/api/ai/*`（M8/M9）、按节点查 MySQL 冷数据的接口（日报 M9 再加）、HTTPS。
