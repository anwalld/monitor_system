# Tasks

## 1. Manager 接口

- [x] 1.1 `web_json`：节点 JSON、事件 JSON、降采样、token 检查、Stream id 比较。验证：gtest 覆盖 720 点降采样、P99 取最大、空桶、缺失 runq 为 null、token 匹配与不匹配
  - 结果：`tests/test_web_json.cpp` 5 个用例通过（3600 个样本 → 720 点、间隔 5 s；乱序输入、P99 取最大、网卡先平均、空桶不输出）
- [x] 1.2 `web_server`（Drogon）：REST 六个接口、WebSocket（先注册再补发、snapshot、node_update、event）、事件泵线程、鉴权；CMake 探测 Drogon，找不到为桩；`main.cpp` 与 `api` 配置。验证：macOS 本机起 Manager + 测试 Redis，curl 各接口；`ws_check.mjs` 断线补发不丢不重
  - 结果：本机 Manager（18080，token tk123）：无 token 401、错 token 1008、`/api/health` 不鉴权；断开期间暂停 Worker → 重连补发 evict + slow_start，与 `/api/events` 一致；Manager 退出 0.59 秒
- [x] 1.3 Linux：builder 与 Manager 镜像加 Drogon；容器内全部测试通过
  - 结果：Ubuntu 的 libdrogon-dev 缺开发依赖，补装 jsoncpp/uuid/zlib/pq/sqlite3/brotli/mariadb 的 -dev 包；MySQL 客户端随之统一为 mariadb（见 m7a design）；222/222 通过

## 2. 看板与 Nginx

- [x] 2.1 `web/`：总览、节点详情、事件页；`live.js` 重连退避与去重；token 输入。验证：`npm run build` 通过；浏览器里三页正常、断开 Manager 后自动重连
  - 结果：构建 597 KB（gzip 206 KB）；内置浏览器里三页正常；重启 Manager 后自动重连，事件 201 条无重复
- [x] 2.2 `nginx.conf`、`web.Dockerfile`、compose `nginx` 服务。验证：镜像构建，经 Nginx 访问看板与 `/ws`
  - 结果：`lm-web` 镜像（node:22-alpine 构建 → nginx:1.27-alpine）；经 Nginx 的 `/`、`/api/`、`/ws` 正常，ws_check 经 Nginx 通过

## 3. 验收

- [x] 3.1 【Linux】部署 Manager 新镜像 + Nginx（测试容器 `lmtest-nginx`），两节点（lm-worker-1 + lmtest-worker2）：总览、详情曲线、事件页正常；暂停 worker2 看到剔除事件 1 秒内出现
  - 结果：虚拟机 Manager 新镜像 + `lmtest-nginx`（host 网络 80 端口）；总览两张卡片（概率 60.6% / 39.4%）、详情曲线、事件页正常（浏览器经 ssh 转发访问）；暂停 worker2 3.0 秒后剔除，事件立即推送
- [x] 3.2 【Linux】断线重连：`ws_check.mjs` 断开 → 期间制造事件（暂停/恢复 worker2、重启 Manager）→ 带 since 重连，与 `/api/events` 对比不丢不重；浏览器看板同样验证
  - 主动断开：断开期间暂停/恢复 worker2，带 since 重连补发 2 条，与 `/api/events` 对比 missing [] extra [] → 通过
  - 服务端重启：`docker compose restart manager`，客户端 1 秒后重连，重启产生的 2 条 slow_start 一条经补发、一条经实时推送 → 通过
  - 浏览器：同一页面经历两次 Manager 重启、一次 nginx 重建，事件页 18 条无重复
- [x] 3.3 更新 README 与 deploy/README（看板、接口、token、Nginx）

## Workflow follow-up

- m7a 与 m7b 都完成后一起提交（M7 一个提交）。
