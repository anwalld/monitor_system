# Spec Delta

## ADDED Requirements

### Requirement: 诊断页
看板 SHALL 提供诊断页：按时间倒序列出 AI 诊断（时间、节点、严重度、一句话结论、触发指标、模型），可按节点和严重度筛选，点开显示可能原因（含证据和置信度）、建议操作和异常详情；收到 `diagnosis` 事件时实时插入。

#### Scenario: 实时出现
- **WHEN** AI 服务写入一条诊断
- **THEN** 诊断页不刷新就出现这一条

### Requirement: 设置页
看板 SHALL 提供设置页：列出 `GET /api/ai/settings` 返回的厂商和模型，标出 Key 是否已配置及来源，提供密码框填写所选厂商的 Key（留空不修改，可删除已填的 Key），保存时调用 `PUT /api/ai/settings`；可以选择“不启用”。

#### Scenario: 切换模型
- **WHEN** 在设置页选择另一个模型并保存
- **THEN** `ai:settings` 更新，下一条诊断的 `model` 是新模型

### Requirement: 事件页与节点详情展示诊断
事件页 SHALL 把 `diagnosis` 事件显示为“AI 诊断”，摘要为严重度和一句话结论；节点详情 SHALL 列出该节点最近 24 小时的诊断。

#### Scenario: AI 服务不可用
- **WHEN** AI 服务停止
- **THEN** 诊断页和设置页显示“AI 服务不可用”，总览、节点详情的其他部分和事件页正常

## MODIFIED Requirements

### Requirement: Nginx 入口
Nginx SHALL 把 `/api/ai/` 反代到 AI 服务（端口 8000），其余 `/api/` 与 `/ws` 反代到 Manager 的 HTTP 端口（WebSocket 升级），`/` 提供看板静态文件。

#### Scenario: 经 Nginx 访问
- **WHEN** 浏览器打开 `http://<manager>/`
- **THEN** 显示看板，WebSocket 经 `/ws` 连上，诊断页经 `/api/ai/` 取数据
