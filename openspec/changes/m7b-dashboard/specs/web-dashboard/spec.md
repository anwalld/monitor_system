# Spec Delta

## Purpose

Web 看板实时展示节点状态、历史曲线和事件，经 Nginx 统一入口访问。

## ADDED Requirements

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
Nginx SHALL 把 `/api/` 与 `/ws` 反代到 Manager 的 HTTP 端口（WebSocket 升级），`/` 提供看板静态文件。

#### Scenario: 经 Nginx 访问
- **WHEN** 浏览器打开 `http://<manager>/`
- **THEN** 显示看板，WebSocket 经 `/ws` 连上
