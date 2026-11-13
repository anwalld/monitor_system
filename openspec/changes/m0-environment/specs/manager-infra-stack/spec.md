# Spec Delta

## Purpose

在 Manager 机上用一条命令拉起调度系统依赖的 Redis 和 MySQL，并自动建好开发文档定义的表结构，同时保证密码等敏感配置不进入版本库。

## ADDED Requirements

### Requirement: 一条命令启动基础设施
在 `deploy/` 目录执行 `docker compose up -d` SHALL 启动 Redis 和 MySQL 两个服务，并为二者配置健康检查与持久化数据卷。Redis MUST 映射 6379 端口，MySQL MUST 映射 3306 端口。

#### Scenario: 首次启动
- **WHEN** 在已安装 Docker Compose v2 的机器上复制 `.env.example` 为 `.env` 后执行 `docker compose up -d`
- **THEN** 两个服务在 60 秒内都变为 healthy，`redis-cli ping` 返回 PONG，MySQL 可用 `.env` 中的账号登录

#### Scenario: 重启后数据保留
- **WHEN** 写入数据后执行 `docker compose down` 再 `docker compose up -d`
- **THEN** 之前写入 MySQL 和 Redis 的数据仍在

### Requirement: 自动初始化表结构
MySQL 首次以空数据卷启动时 SHALL 自动创建数据库及 `node`、`node_metrics_5m`、`node_event`、`task`、`ai_diagnosis`、`ai_daily_report` 六张表，字段、主键和索引与开发文档一致，字符集为 utf8mb4。

#### Scenario: 表结构就绪
- **WHEN** MySQL 首次启动完成后执行 `SHOW TABLES`
- **THEN** 结果恰好包含上述六张表，且 `SHOW CREATE TABLE task` 显示主键 `task_id CHAR(26)` 和索引 `idx_node_alloc (node_id, t_alloc)`

### Requirement: 敏感配置不进版本库
数据库密码等敏感配置 MUST 只从 `deploy/.env` 读取；仓库 SHALL 提供不含真实密码的 `deploy/.env.example`，且 `.env` MUST 被 `.gitignore` 忽略。

#### Scenario: .env 被忽略
- **WHEN** 在 `deploy/` 下创建 `.env` 后执行 `git status`
- **THEN** `.env` 不出现在未跟踪文件中

#### Scenario: 缺少 .env
- **WHEN** 未创建 `.env` 直接执行 `docker compose up -d`
- **THEN** Compose 报出缺少的变量名并拒绝启动 MySQL，而不是用空密码启动
