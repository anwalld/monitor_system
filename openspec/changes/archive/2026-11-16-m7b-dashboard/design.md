# Design

## Context

- 节点的实时状态在 Manager 内存的 `NodeTable` 里（最新样本、分数明细、状态、概率），Redis 的 `node:{id}` 是它的副本。
- 曲线数据在 `node:{id}:ts`（每秒一条压缩 JSON，proto 字段名，保留 1 小时）；事件在 `stream:events`（m7a）。
- Manager 是 host 网络，Nginx 也用 host 网络，经 127.0.0.1:8080 反代。

## Goals / Non-Goals

**Goals**
- 看板只经 Manager 读数据，事件只来自 `stream:events` 一个源，断线重连不丢不重。
- 接口线程与调度、存储完全分开；Redis 挂掉时总览照常（来自内存），曲线和事件暂时为空。
- 降采样、JSON 生成、鉴权判断都是可单测的纯函数。

**Non-Goals**
- AI 页面、HTTPS、多用户权限。

## Decisions

### 1. 模块

```
manager/api/
  web_json.{h,cpp}      纯函数：节点 JSON、事件 JSON、降采样、token 检查（不依赖 Drogon，总是编译和测试）
  web_server.{h,cpp}    Drogon：路由、WebSocket 连接集合、事件泵线程、每秒 node_update；无 Drogon 时为桩
web/                    Vue 3 + Vite + ECharts
deploy/nginx/nginx.conf、deploy/docker/web.Dockerfile
experiments/ws_check.mjs
```

### 2. 线程

```
Drogon IO 线程（2 个）：REST 处理、WebSocket 收发（处理函数里同步访问 Redis，超时 500 ms）
事件泵线程：XREAD BLOCK 1000 stream:events，广播 event
推送线程：每秒取 NodeTable 快照 → node_update 广播
```

- Drogon 的 `app()` 是进程级单例，`run()` 阻塞，所以放在单独线程里跑；退出时 `app().quit()` 再 join。关闭 Drogon 自己的 SIGTERM 处理（`disableSigtermHandling`），信号仍由 main 处理。
- 连接集合用一把小锁保护；`WebSocketConnection::send` 是线程安全的（内部投递到连接所属的事件循环）。
- 事件泵和 REST 各用一个 `sw::redis::Redis`（连接池），事件泵的 socket 超时大于 BLOCK 时间。Redis 不可用时事件泵每秒重试，限频记日志。
- 事件泵启动时从 `XREVRANGE + - COUNT 1` 取最新 id 开始读（不重放历史）；之后每次 XREAD 从上次读到的 id 继续，不用 `$`，避免两次调用之间漏事件。

### 3. WebSocket 协议与补发

- `snapshot`：`{"type":"snapshot","ts","last_event_id","nodes":[…]}`；`node_update`：`{"type":"node_update","ts","nodes":[…]}`；`event`：`{"type":"event","id","node","kind","ts","detail":{…}}`（detail 原样嵌入 JSON）。
- 新连接：**先加入广播集合，再读补发**。带 `since` 时 `XRANGE stream:events (since +`（不含 since 本身，最多 10000 条）逐条发 `event`，然后发 `snapshot`（`last_event_id` 取此刻的最新 id）。先注册保证补发和实时推送之间没有空档；两者可能重叠，由前端按 id 去重——这就是“不丢不重”的分工：服务端保证不丢，客户端保证不重。
- `since` 不合法或已被 MAXLEN 修剪掉：照常从流里现有最早的开始补发（XRANGE 本身就是这个语义），不报错。
- 每秒的 `node_update` 只在有连接时生成。

### 4. 节点对象

```json
{"id":"w1","addr":"…:50052","ncpu":4,"online":true,"age_ms":420,
 "state":"normal","state_reason":"","state_since_ms":…,"f_slow":1,
 "score":0.82,"score_parts":{"cpu":…,"runq":…,"mem":…,"net":…,"best_nic":"ens160"},
 "evicted":false,"evict_reason":"","prob":0.41,
 "cpu":0.33,"mem":0.62,"disk":0.01,"runq_p99_us":180,
 "inflight":{"inflight":2,"f_inflight":…,"f_timeout":…,"timeout_count":…},
 "nics":[{"name":"ens160","rtt_ms":…,"timeout_rate":…,"retrans_rate":…,"rx_bps":…,"tx_bps":…}]}
```

- 来自 `NodeTable` 快照（内存），不读 Redis。没有调度延迟时 `runq_p99_us` 为 null。
- REST `/api/nodes` 与 WebSocket 用同一个生成函数。

### 5. 曲线降采样

- `GET /api/nodes/{id}/series?minutes=60&step_s=5`（minutes 1–60，step_s 1–60）：`ZRANGEBYSCORE node:{id}:ts now−minutes now`，每个 member 用 protobuf 的 `JsonStringToMessage` 解析回 `lm::Sample`（不引入新的 JSON 解析库）。
- 按 `floor(ts/step)×step` 分桶，桶内：cpu、mem、disk 取平均；`runq_p99_us` 取最大（P99 不能平均，取最坏的更保守）；网络每个样本先对各网卡平均，桶内再平均；rx/tx 取平均。空桶不输出（前端按时间轴画，缺口自然断开）。
- 1 小时 × 每 5 秒 = 最多 720 点。
- 曲线不含分数：分数不在 `node:{id}:ts` 里，总览和详情页显示当前分数明细即可。

### 6. 鉴权

- token 为空：全部放行，启动时 warn 一次。
- 非空：REST 检查 `Authorization: Bearer <token>`，WebSocket 还接受 `?token=`（浏览器 WebSocket 不能设请求头）。不匹配返回 401 / 拒绝升级。比较用定长循环，不提前返回。
- `/api/health` 不鉴权（给 Nginx、compose 健康检查用）。

### 7. 看板

- Vite + Vue 3（组合式 API）+ vue-router（hash 模式，Nginx 不用改 rewrite）+ ECharts（按需引入 line 图）。不用 UI 组件库，手写少量 CSS，深色/浅色跟随系统。
- `src/live.js`：单例连接管理。状态 `connecting / open / closed`；断开后按 1、2、4、8、16、30、30… 秒重连，连上后退避归零；重连带 `since=<lastEventId>`；事件存在 `Map<id, event>`（最多 2000 条，超出删最旧），`lastEventId` 取收到的最大 id（Stream id 按 `ms-seq` 比较）。
- token：首次打开 `/#/?token=xxx` 或在页面右上角输入，存 localStorage；401 时提示输入。
- 页面：
  - 总览：每台节点一张卡片（状态色、分数、概率、CPU、P99、内存、各网卡 RTT/超时率），点进详情。
  - 节点详情：CPU/内存/磁盘、调度延迟 P99、网卡 RTT 与超时率三张曲线（每 10 秒刷新），分数明细，该节点事件时间线（REST 初始 + 实时追加）。
  - 事件：全部事件时间流，按节点、种类筛选；顶部显示连接状态与重连次数。
- 开发：`npm run dev` 时 Vite 把 `/api`、`/ws` 代理到 `VITE_MANAGER`（默认 http://127.0.0.1:8080）。

### 8. 部署

- `web.Dockerfile`：`node:22-alpine` 构建 → `nginx:1.27-alpine`，拷贝 `dist/` 与 `nginx.conf`。
- `nginx.conf`：`listen ${HTTP_PORT}`（envsubst 模板），`/api/` 与 `/ws` → `127.0.0.1:${MANAGER_HTTP_PORT}`（WebSocket 升级头、`proxy_read_timeout 3600s`），`/` → 静态文件，`index.html` 不缓存、带 hash 的资源长缓存。
- compose：`nginx` 服务 host 网络；Manager 加 `LM_API_TOKEN: ${API_TOKEN:-}`，Manager 镜像运行阶段加 Drogon 运行库，`EXPOSE 8080`。
- 配置 `api.enabled`（默认 true）、`api.listen`（默认 `0.0.0.0:8080`）、`api.token`、`api.threads`（2）。

## 性能与资源影响

- 采集、上报、调度不变。每秒一次节点表快照（≤ 20 台的拷贝，锁内微秒级）。
- Redis：事件泵一条阻塞连接；曲线请求一次 ZRANGEBYSCORE（3600 条，约几百 KB），详情页每 10 秒一次。
- 内存：Drogon 两个 IO 线程；每个 WebSocket 连接的发送缓冲。

## Risks / Trade-offs

- **慢客户端**：Drogon 的发送缓冲会增长。看板连接数少（个位数），不额外限流；以后需要时按缓冲大小断开。
- **补发上限**：断线时间长到事件超过 MAXLEN 1 万条时，补发只能从流里最早的开始，更早的事件在 MySQL `node_event` 里。
- **REST 在 IO 线程里同步访问 Redis**：Redis 卡住时最多卡 500 ms（超时），只影响看板。

## Migration Plan

- compose 新增 `nginx` 服务，Manager 增加 `LM_API_TOKEN`；Manager 镜像运行阶段增加 Drogon 运行库。
- 回滚：换回 m6 / m7a 的 Manager 镜像，停掉 nginx。

## 实现中的决定（2026-11-14）

- **Ubuntu 的 libdrogon-dev 缺开发依赖**：它的 CMake 配置 `find_dependency` 了 jsoncpp、uuid、zlib，链接接口还引用 pq、sqlite3、mariadb、brotli，但包本身不依赖这些 `-dev` 包。builder 与 Manager 镜像构建阶段显式安装 `libjsoncpp-dev uuid-dev zlib1g-dev libpq-dev libsqlite3-dev libbrotli-dev libmariadb-dev`；运行阶段只加 `libdrogon1t64`（其余运行库由它带入）。
- **Vite 开发代理**：`/ws` 的 target 写 http 地址加 `ws: true`，写成 ws:// 地址时代理不生效。
- **鉴权失败的关闭码**：服务端在升级后 `shutdown(kViolation)`，浏览器收到 1008，看板据此显示“token 无效”并停止重连，等用户输入。
- **本机 8080 被其他项目占用**：Mac 上测试用 `api.listen: 127.0.0.1:18080`（测试配置在会话临时目录），默认配置不变。
