# Spec Delta

## ADDED Requirements

### Requirement: 问答、预测与日报接口
AI 服务 SHALL 新增 `POST /api/ai/chat`（SSE，见 `ai-ops-chat`）、`GET /api/ai/predictions`（见 `ai-load-prediction`）、`GET /api/ai/reports`、`GET /api/ai/reports/{day}`、`POST /api/ai/reports/{day}`（见 `ai-daily-report`）。鉴权与既有接口相同；`/api/ai/health` 增加 `predict`（enabled、push、nodes、mae_holt、mae_naive）与 `report`（last_day、next_run）字段。M8 的接口不变。

#### Scenario: 鉴权
- **WHEN** 配置了 `API_TOKEN` 而请求 `POST /api/ai/chat` 没带 token
- **THEN** 返回 401
