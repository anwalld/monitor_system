# Spec Delta

## MODIFIED Requirements

### Requirement: Probe
`WorkerTask.Probe(ProbeReq{seq})` SHALL 在任务服务开启时立即返回 `ProbeResp{seq}`，不校验 token。`WorkerTask.Rescue` 的行为见 `worker-rescue`。

#### Scenario: 探测
- **WHEN** 调用 Probe，seq 为 42
- **THEN** 返回 seq 42
