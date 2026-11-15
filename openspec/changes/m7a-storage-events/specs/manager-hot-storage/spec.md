# Spec Delta

## ADDED Requirements

### Requirement: 节点 Hash 中的选中概率
Manager SHALL 每 100 ms 计算每台节点的选中概率（候选节点为其权重占总权重的比例，非候选为 0），并在写 `node:{id}` 时写入 `prob`（小数点后 4 位）。

#### Scenario: 两台节点
- **WHEN** w1、w2 都正常，权重分别为 0.6、0.4
- **THEN** `node:w1` 的 `prob` 为 `0.6000`，`node:w2` 的为 `0.4000`
