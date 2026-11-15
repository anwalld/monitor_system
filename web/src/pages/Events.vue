<script setup>
import { computed, onMounted, ref } from "vue";
import { api, addEvent, live } from "../live.js";
import { datetime, KIND_LABEL, eventSummary } from "../fmt.js";

const node = ref("");
const kind = ref("");
const loading = ref(false);

// 打开页面时用 REST 补齐最近 200 条（与 WebSocket 推送按 id 去重合并）
onMounted(async () => {
  loading.value = true;
  try {
    for (const e of (await api("/api/events?limit=200")).events) addEvent(e);
  } catch {}
  loading.value = false;
});

const nodeIds = computed(() => [...new Set([...live.nodes.map((n) => n.id), ...live.events.map((e) => e.node)])].sort());
const kinds = computed(() => [...new Set(live.events.map((e) => e.kind))].sort());
const shown = computed(() =>
  live.events.filter((e) => (!node.value || e.node === node.value) && (!kind.value || e.kind === kind.value)),
);
</script>

<template>
  <div class="filters">
    <select v-model="node">
      <option value="">全部节点</option>
      <option v-for="n in nodeIds" :key="n" :value="n">{{ n }}</option>
    </select>
    <select v-model="kind">
      <option value="">全部种类</option>
      <option v-for="k in kinds" :key="k" :value="k">{{ KIND_LABEL[k] || k }}</option>
    </select>
    <span class="muted">共 {{ shown.length }} 条（已去重，连接 {{ live.connects }} 次，最新 id {{ live.lastEventId || "-" }}）</span>
  </div>
  <div class="card">
    <div v-if="!shown.length" class="empty">{{ loading ? "加载中…" : "暂无事件" }}</div>
    <table v-else>
      <thead><tr><th>时间</th><th>节点</th><th>事件</th><th>详情</th><th>id</th></tr></thead>
      <tbody>
        <tr v-for="e in shown" :key="e.id">
          <td class="num">{{ datetime(e.ts) }}</td>
          <td><router-link :to="`/nodes/${encodeURIComponent(e.node)}`">{{ e.node }}</router-link></td>
          <td :class="`k-${e.kind}`">{{ KIND_LABEL[e.kind] || e.kind }}</td>
          <td>
            <router-link v-if="e.kind === 'diagnosis' && e.detail.diag_id" :to="`/diagnoses?id=${e.detail.diag_id}&node=${encodeURIComponent(e.node)}`">{{ eventSummary(e) }}</router-link>
            <template v-else>{{ eventSummary(e) }}</template>
          </td>
          <td class="muted num">{{ e.id }}</td>
        </tr>
      </tbody>
    </table>
  </div>
</template>
