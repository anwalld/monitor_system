<script setup>
import { computed, onBeforeUnmount, onMounted, ref, watch } from "vue";
import Chart from "../components/Chart.vue";
import { api, live, onEvent, idLess } from "../live.js";
import { pct, num, us, datetime, time, STATE_LABEL, KIND_LABEL, eventSummary } from "../fmt.js";

const props = defineProps({ id: String });
const points = ref([]);
const events = ref([]);
const err = ref("");
const node = computed(() => live.nodes.find((n) => n.id === props.id));

async function loadSeries() {
  try {
    points.value = (await api(`/api/nodes/${encodeURIComponent(props.id)}/series?minutes=60&step_s=5`)).points;
    err.value = "";
  } catch (e) {
    err.value = `曲线加载失败：${e.message}`;
  }
}
async function loadEvents() {
  try {
    const got = (await api(`/api/nodes/${encodeURIComponent(props.id)}/events?limit=100`)).events;
    const ids = new Set(events.value.map((e) => e.id));
    events.value = [...events.value, ...got.filter((e) => !ids.has(e.id))].sort((a, b) => (idLess(a.id, b.id) ? 1 : -1));
  } catch {}
}

let timer = null;
let off = null;
onMounted(() => {
  loadSeries();
  loadEvents();
  timer = setInterval(loadSeries, 10000);
  off = onEvent((e) => {
    if (e.node === props.id && !events.value.some((x) => x.id === e.id)) events.value.unshift(e);
  });
});
onBeforeUnmount(() => {
  clearInterval(timer);
  off?.();
});
watch(() => props.id, () => {
  points.value = [];
  events.value = [];
  loadSeries();
  loadEvents();
});

const s = (k, scale = 1) => points.value.map((p) => [p.ts, p[k] == null ? null : p[k] * scale]);
const cpuSeries = computed(() => [
  { name: "CPU", data: s("cpu") },
  { name: "可用内存", data: s("mem") },
  { name: "磁盘", data: s("disk") },
]);
const runqSeries = computed(() => [{ name: "P99（µs）", data: s("runq_p99_us") }]);
const netSeries = computed(() => [
  { name: "RTT（ms）", data: s("rtt_ms") },
  { name: "超时率", data: s("timeout_rate"), yAxisIndex: 1 },
  { name: "重传率", data: s("retrans_rate"), yAxisIndex: 1 },
]);
const trafficSeries = computed(() => [
  { name: "接收", data: s("rx_bps", 8 / 1e6) },
  { name: "发送", data: s("tx_bps", 8 / 1e6) },
]);
const pctFmt = (v) => `${Math.round(v * 100)}%`;
</script>

<template>
  <p><router-link to="/" class="muted">← 总览</router-link></p>
  <div v-if="!node" class="empty">节点 {{ id }} 不在当前节点表中</div>
  <template v-else>
    <div class="card">
      <div class="row">
        <h3>{{ node.id }}</h3>
        <span class="badge" :class="node.online ? `s-${node.state}` : 's-offline'">
          {{ STATE_LABEL[node.state] || node.state }}{{ node.online ? "" : " · 失联" }}
        </span>
      </div>
      <div class="muted">
        {{ node.addr }} · {{ node.ncpu }} 核 · 首次登记 {{ datetime(node.first_seen_ms) }} · 当前状态始于
        {{ datetime(node.state_since_ms) }}{{ node.state_reason ? `（${node.state_reason}）` : "" }}
      </div>
      <table style="margin-top: 10px">
        <thead>
          <tr><th>总分</th><th>CPU 分</th><th>调度延迟分</th><th>内存分</th><th>网络分</th><th>慢启动系数</th>
            <th>在途系数</th><th>超时系数（T）</th><th>选中概率</th></tr>
        </thead>
        <tbody>
          <tr>
            <td class="num">{{ num(node.score, 3) }}</td>
            <td class="num">{{ num(node.score_parts.cpu, 3) }}</td>
            <td class="num">{{ num(node.score_parts.runq, 3) }}</td>
            <td class="num">{{ num(node.score_parts.mem, 3) }}</td>
            <td class="num">{{ num(node.score_parts.net, 3) }}{{ node.score_parts.best_nic ? `（${node.score_parts.best_nic}）` : "" }}</td>
            <td class="num">{{ num(node.f_slow, 3) }}</td>
            <td class="num">{{ num(node.inflight.f_inflight, 3) }}（{{ node.inflight.inflight }} 个）</td>
            <td class="num">{{ num(node.inflight.f_timeout, 3) }}（{{ num(node.inflight.timeout_count) }}）</td>
            <td class="num">{{ pct(node.prob) }}</td>
          </tr>
        </tbody>
      </table>
      <p class="muted">当前：CPU {{ pct(node.cpu) }} · 可用内存 {{ pct(node.mem) }} · 调度延迟 P99 {{ us(node.runq_p99_us) }}</p>
    </div>
  </template>

  <p v-if="err" class="muted">{{ err }}</p>
  <div class="charts">
    <Chart title="CPU / 内存 / 磁盘（最近 1 小时，每 5 秒）" :series="cpuSeries" :y-axes="[{ max: 1, fmt: pctFmt }]" />
    <Chart title="调度延迟 P99（桶内最大）" :series="runqSeries" :y-axes="[{ name: 'µs' }]" />
    <Chart title="网卡（各网卡平均）" :series="netSeries" :y-axes="[{ name: 'ms' }, { name: '比例', max: 1 }]" />
    <Chart title="流量（Mbit/s，各网卡之和）" :series="trafficSeries" :y-axes="[{ name: 'Mbit/s' }]" />
  </div>

  <div class="card">
    <h3>状态时间线</h3>
    <div v-if="!events.length" class="empty">暂无事件</div>
    <table v-else>
      <tbody>
        <tr v-for="e in events" :key="e.id">
          <td class="num">{{ datetime(e.ts) }}</td>
          <td :class="`k-${e.kind}`">{{ KIND_LABEL[e.kind] || e.kind }}</td>
          <td>{{ eventSummary(e) }}</td>
        </tr>
      </tbody>
    </table>
  </div>
</template>
