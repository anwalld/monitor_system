export const pct = (v) => (v == null ? "—" : `${(v * 100).toFixed(1)}%`);
export const num = (v, d = 2) => (v == null ? "—" : Number(v).toFixed(d));
export const us = (v) => (v == null ? "—" : v >= 1000 ? `${(v / 1000).toFixed(1)} ms` : `${Math.round(v)} µs`);
export const time = (ms) => new Date(ms).toLocaleTimeString("zh-CN", { hour12: false });
export const datetime = (ms) => new Date(ms).toLocaleString("zh-CN", { hour12: false });
export const ago = (ms) => {
  const s = Math.max(0, Math.round((Date.now() - ms) / 1000));
  if (s < 60) return `${s} 秒前`;
  if (s < 3600) return `${Math.floor(s / 60)} 分钟前`;
  return `${Math.floor(s / 3600)} 小时前`;
};
export const STATE_LABEL = { normal: "正常", slow_start: "慢启动", evicted: "剔除", rescue: "急救" };
export const KIND_LABEL = {
  slow_start: "慢启动", normal: "恢复正常", evict: "剔除", rescue_start: "开始急救", rescue_end: "急救结束",
  diagnosis: "AI 诊断",
};
export const SEV_LABEL = { high: "高", medium: "中", low: "低" };
export const LLM_REASON = {
  ok: "LLM", disabled: "未启用 LLM", no_key: "缺少 API Key", budget: "超出当日 token 预算",
  invalid: "LLM 输出两次不合格", unknown_vendor: "厂商不在列表", unknown_model: "模型不在列表",
};
export const llmReason = (r) => (r ? LLM_REASON[r] || (r.startsWith("error") ? `LLM 调用失败（${r.slice(7)}）` : r) : "—");
export function eventSummary(e) {
  const d = e.detail || {};
  switch (e.kind) {
    case "slow_start":
      return d.from === "new" ? `新节点登记 ${d.addr || ""}（${d.ncpu ?? "?"} 核）` : `从${STATE_LABEL[d.from] || d.from}进入慢启动`;
    case "normal": return "慢启动结束";
    case "evict": return `从${STATE_LABEL[d.from] || d.from}剔除，原因 ${d.reason || "?"}`;
    case "rescue_start": return `选中概率 ${num(d.prob, 3)}，候选 ${d.candidates}，T ${num(d.timeout_count)}，退避 ${d.backoff_s} 秒`;
    case "rescue_end": return `${d.result}，${d.ms} ms，批 ${d.batches_ok}/${d.batches_fail}，T ${num(d.t_before)} → ${num(d.t_after)}，转为 ${STATE_LABEL[d.to] || d.to}`;
    case "diagnosis": return `[${SEV_LABEL[d.severity] || d.severity}] ${d.summary || ""}`;
    default: return JSON.stringify(d);
  }
}
