# Spec Delta

## ADDED Requirements

### Requirement: 节点 JSON 含预测
`GET /api/nodes`、`GET /api/nodes/{id}` 和 WebSocket `node_update` 里每个节点 SHALL 含 `f_pred`（数值）与 `pred_cpu`（无预测时为 null）。

#### Scenario: 有预测
- **WHEN** Manager 10 秒内收到该节点因子 0.75、预测 0.85
- **THEN** 节点 JSON 里 `f_pred = 0.75`、`pred_cpu = 0.85`
