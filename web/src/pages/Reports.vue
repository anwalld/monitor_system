<script setup>
// 日报（M9）：日期列表 + Markdown 正文（LLM 写的或数字表格）+ 按 stats 渲染的数字表格。
import { computed, onMounted, ref } from "vue";
import { marked } from "marked";
import { api, aiDown } from "../live.js";
import { datetime, llmReason } from "../fmt.js";

const list = ref([]);
const day = ref("");
const rep = ref(null);
const err = ref("");
const down = ref(false);
const busy = ref(false);

marked.use({ renderer: { html: () => "" } }); // 不直通 HTML

async function loadList() {
  try {
    list.value = (await api("/api/ai/reports?limit=60")).reports;
    down.value = false;
    err.value = "";
    if (!day.value && list.value.length) open(list.value[0].day);
  } catch (e) {
    down.value = aiDown(e);
    err.value = e.message;
  }
}
async function open(d) {
  day.value = d;
  rep.value = null;
  try {
    rep.value = await api(`/api/ai/reports/${d}`);
    err.value = "";
  } catch (e) {
    err.value = e.status === 404 ? "这一天还没有日报" : e.message;
  }
}
async function regen(d) {
  if (!d) return;
  busy.value = true;
  try {
    rep.value = await api(`/api/ai/reports/${d}`, { method: "POST" });
    day.value = d;
    err.value = "";
    await loadList();
  } catch (e) {
    err.value = `生成失败：${e.message}`;
  }
  busy.value = false;
}
const yesterday = () => {
  const t = new Date(Date.now() - 86400000);
  return `${t.getFullYear()}-${String(t.getMonth() + 1).padStart(2, "0")}-${String(t.getDate()).padStart(2, "0")}`;
};
const pick = ref(yesterday());
const html = computed(() => (rep.value ? marked.parse(rep.value.markdown || "") : ""));
const st = computed(() => rep.value?.stats || {});
const sourceText = computed(() => {
  const r = st.value.llm;
  if (r === "ok") return `LLM 撰写（${st.value.model}，${st.value.tokens} tokens），正文数字已逐个核对`;
  if (r === "mismatch") return `LLM 正文有数字对不上统计（${(st.value.mismatched || []).join("、")}），只显示数字表格`;
  return `未使用 LLM（${llmReason(r)}），只显示数字表格`;
});
const dash = (v) => (v == null ? "—" : v);
onMounted(loadList);
</script>

<template>
  <div v-if="down" class="notice">AI 服务不可用（{{ err }}）。调度和其他页面不受影响。</div>
  <div v-else class="reports">
    <aside class="card">
      <h3>日期</h3>
      <div class="gen">
        <input v-model="pick" type="date" />
        <button :disabled="busy" @click="regen(pick)">{{ busy ? "生成中…" : "生成" }}</button>
      </div>
      <div v-if="!list.length" class="muted">还没有日报（每天 09:00 自动生成前一天的）</div>
      <a v-for="r in list" :key="r.day" href="#" class="day" :class="{ on: r.day === day }" @click.prevent="open(r.day)">
        {{ r.day }}<span class="muted">{{ r.llm === "ok" ? "LLM" : r.llm === "mismatch" ? "表格（核对未过）" : "表格" }}</span>
      </a>
    </aside>
    <section class="card body">
      <div v-if="err" class="muted">{{ err }}</div>
      <template v-if="rep">
        <div class="row">
          <span class="muted">{{ sourceText }} · 生成于 {{ datetime(rep.created_at) }}</span>
          <button :disabled="busy" @click="regen(rep.day)">重新生成</button>
        </div>
        <article class="md" v-html="html"></article>
        <h4>数字（程序从 MySQL 算出）</h4>
        <table>
          <thead><tr><th>节点</th><th>可用率</th><th>CPU 平均 / 最大</th><th>剔除</th><th>急救</th><th>任务</th>
            <th>超时</th><th>P50 / P99 / 最大（ms）</th></tr></thead>
          <tbody>
            <tr v-for="n in st.nodes" :key="n.node">
              <td>{{ n.node }}</td>
              <td class="num">{{ n.avail_pct }}%（{{ n.avail_h }} h）</td>
              <td class="num">{{ dash(n.cpu_avg_pct) }}% / {{ dash(n.cpu_max_pct) }}%</td>
              <td class="num">{{ n.evictions }}</td>
              <td class="num">{{ n.rescues }}</td>
              <td class="num">{{ n.tasks }}</td>
              <td class="num">{{ n.timeouts }}<span v-if="n.unconfirmed" class="muted">（确认超时 {{ n.unconfirmed }}）</span></td>
              <td class="num">{{ dash(n.p50_ms) }} / {{ dash(n.p99_ms) }} / {{ dash(n.max_ms) }}</td>
            </tr>
            <tr v-if="st.totals" class="total">
              <td>合计</td><td></td><td></td>
              <td class="num">{{ st.totals.evictions }}</td>
              <td class="num">{{ st.totals.rescues }}</td>
              <td class="num">{{ st.totals.tasks }}</td>
              <td class="num">{{ st.totals.timeouts }}<span v-if="st.totals.unconfirmed" class="muted">（确认超时 {{ st.totals.unconfirmed }}）</span></td>
              <td class="num">{{ dash(st.totals.p50_ms) }} / {{ dash(st.totals.p99_ms) }} / {{ dash(st.totals.max_ms) }}</td>
            </tr>
          </tbody>
        </table>
        <p class="muted">
          AI 诊断 {{ st.diagnoses?.total || 0 }} 条（高 {{ st.diagnoses?.high || 0 }}、中 {{ st.diagnoses?.medium || 0 }}、低
          {{ st.diagnoses?.low || 0 }}）。可用率 = 当天收到的每秒样本数 / 当天秒数；完成时间只统计 done 任务（Worker 收到到完成）；超时含执行超时和确认超时（分配后 Worker 没确认收到）。
        </p>
      </template>
    </section>
  </div>
</template>

<style scoped>
.reports { display: grid; grid-template-columns: 260px 1fr; gap: 14px; align-items: start; }
.day { display: flex; justify-content: space-between; gap: 6px; padding: 5px 8px; border-radius: 6px; white-space: nowrap; }
.day.on { background: var(--bg); font-weight: 600; }
.gen { display: flex; gap: 6px; margin: 8px 0 10px; }
.gen input { min-width: 0; flex: 1; }
.body { min-width: 0; }
.md { line-height: 1.7; }
.md :deep(table) { margin: 8px 0; }
.md :deep(blockquote) { margin: 6px 0; padding: 4px 12px; border-left: 3px solid var(--warn); color: var(--muted); }
tr.total td { font-weight: 600; }
@media (max-width: 760px) { .reports { grid-template-columns: 1fr; } }
</style>
