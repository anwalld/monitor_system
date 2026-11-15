<script setup>
import { computed, onBeforeUnmount, onMounted, ref, watch } from "vue";
import { useRoute } from "vue-router";
import DiagDetail from "../components/DiagDetail.vue";
import { api, aiDown, live, onEvent } from "../live.js";
import { datetime, SEV_LABEL, llmReason } from "../fmt.js";

const route = useRoute();
const node = ref(route.query.node || "");
const severity = ref("");
const hours = ref(24);
const rows = ref([]);
const open = ref(Number(route.query.id) || null);
const err = ref("");
const down = ref(false);
const loading = ref(false);

async function load() {
  loading.value = true;
  const q = new URLSearchParams({ hours: hours.value, limit: 500 });
  if (node.value) q.set("node", node.value);
  if (severity.value) q.set("severity", severity.value);
  try {
    rows.value = (await api(`/api/ai/diagnoses?${q}`)).diagnoses;
    err.value = "";
    down.value = false;
  } catch (e) {
    down.value = aiDown(e);
    err.value = e.message;
  }
  loading.value = false;
}

let off = null;
let timer = null;
onMounted(() => {
  load();
  // 诊断事件实时到达：重新拉列表（事件里只有摘要，列表需要完整的一行）
  off = onEvent((e) => {
    if (e.kind === "diagnosis") {
      clearTimeout(timer);
      timer = setTimeout(load, 300);
    }
  });
});
onBeforeUnmount(() => {
  off?.();
  clearTimeout(timer);
});
watch([node, severity, hours], load);

const nodeIds = computed(() => [...new Set([...live.nodes.map((n) => n.id), ...rows.value.map((r) => r.node)])].sort());
const toggle = (id) => (open.value = open.value === id ? null : id);
</script>

<template>
  <div class="filters">
    <select v-model="node">
      <option value="">全部节点</option>
      <option v-for="n in nodeIds" :key="n" :value="n">{{ n }}</option>
    </select>
    <select v-model="severity">
      <option value="">全部严重度</option>
      <option value="high">高</option>
      <option value="medium">中</option>
      <option value="low">低</option>
    </select>
    <select v-model.number="hours">
      <option :value="1">最近 1 小时</option>
      <option :value="24">最近 24 小时</option>
      <option :value="168">最近 7 天</option>
    </select>
    <span class="muted">共 {{ rows.length }} 条 · 诊断只给建议，不自动执行</span>
  </div>
  <div v-if="down" class="notice">AI 服务不可用（{{ err }}）。调度和其他页面不受影响。</div>
  <div v-else class="card">
    <div v-if="err" class="muted">加载失败：{{ err }}</div>
    <div v-if="!rows.length" class="empty">{{ loading ? "加载中…" : "暂无诊断" }}</div>
    <table v-else>
      <thead><tr><th>时间</th><th>节点</th><th>严重度</th><th>结论</th><th>触发</th><th>来源</th></tr></thead>
      <tbody>
        <template v-for="r in rows" :key="r.id">
          <tr class="click" @click="toggle(r.id)">
            <td class="num">{{ datetime(r.ts) }}</td>
            <td><router-link :to="`/nodes/${encodeURIComponent(r.node)}`" @click.stop>{{ r.node }}</router-link></td>
            <td><span class="badge" :class="`sev-${r.severity}`">{{ SEV_LABEL[r.severity] || r.severity }}</span></td>
            <td>{{ r.summary }}</td>
            <td class="muted">{{ r.label || r.metric }}</td>
            <td class="muted">{{ r.model || llmReason(r.llm) }}</td>
          </tr>
          <tr v-if="open === r.id"><td colspan="6"><DiagDetail :id="r.id" /></td></tr>
        </template>
      </tbody>
    </table>
  </div>
</template>
