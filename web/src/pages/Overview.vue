<script setup>
import { computed } from "vue";
import { live } from "../live.js";
import { pct, num, us, ago, STATE_LABEL } from "../fmt.js";

const nodes = computed(() => live.nodes);
const totals = computed(() => {
  const c = { total: live.nodes.length, online: 0, normal: 0, slow_start: 0, evicted: 0, rescue: 0 };
  for (const n of live.nodes) {
    if (n.online) c.online++;
    c[n.state] = (c[n.state] || 0) + 1;
  }
  return c;
});
const stateClass = (n) => (n.online ? `s-${n.state}` : "s-offline");
const stateText = (n) => (n.online ? STATE_LABEL[n.state] || n.state : `${STATE_LABEL[n.state] || n.state} · 失联`);
</script>

<template>
  <p class="muted">
    节点 {{ totals.total }}，在线 {{ totals.online }}：正常 {{ totals.normal }}、慢启动 {{ totals.slow_start }}、剔除
    {{ totals.evicted }}、急救 {{ totals.rescue }}
  </p>
  <div v-if="!nodes.length" class="empty">还没有节点数据</div>
  <div class="grid">
    <router-link v-for="n in nodes" :key="n.id" :to="`/nodes/${encodeURIComponent(n.id)}`" class="card">
      <div class="row">
        <h3>{{ n.id }}</h3>
        <span class="badge" :class="stateClass(n)">{{ stateText(n) }}</span>
      </div>
      <div class="muted">{{ n.addr }} · {{ n.ncpu }} 核 · {{ n.online ? `${n.age_ms} ms 前上报` : ago(n.last_seen_ms) }}</div>
      <div class="bar" :title="`选中概率 ${pct(n.prob)}`"><i :style="{ width: `${(n.prob * 100).toFixed(1)}%` }"></i></div>
      <dl class="kv">
        <dt>分数</dt><dd>{{ num(n.score, 3) }}</dd>
        <dt>选中概率</dt><dd>{{ pct(n.prob) }}</dd>
        <dt>CPU</dt><dd>{{ pct(n.cpu) }}</dd>
        <dt>调度延迟 P99</dt><dd>{{ us(n.runq_p99_us) }}</dd>
        <dt>可用内存</dt><dd>{{ pct(n.mem) }}</dd>
        <dt>在途任务</dt><dd>{{ n.inflight.inflight }}（T {{ num(n.inflight.timeout_count) }}）</dd>
        <template v-for="c in n.nics" :key="c.name">
          <dt>{{ c.name }}</dt>
          <dd>{{ num(c.rtt_ms, 1) }} ms · 超时 {{ pct(c.timeout_rate) }}</dd>
        </template>
        <template v-if="n.state_reason">
          <dt>原因</dt><dd>{{ n.state_reason }}</dd>
        </template>
      </dl>
    </router-link>
  </div>
</template>
