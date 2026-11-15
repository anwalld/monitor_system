<script setup>
// 一条诊断的详情：可能原因、建议操作、触发异常、上下文（按需加载）
import { onMounted, ref } from "vue";
import { api } from "../live.js";
import { num, llmReason } from "../fmt.js";

const props = defineProps({ id: Number });
const d = ref(null);
const err = ref("");
const showCtx = ref(false);

onMounted(async () => {
  try {
    d.value = await api(`/api/ai/diagnoses/${props.id}`);
  } catch (e) {
    err.value = `加载失败：${e.message}`;
  }
});
</script>

<template>
  <div class="detail">
    <div v-if="err" class="muted">{{ err }}</div>
    <div v-else-if="!d" class="muted">加载中…</div>
    <template v-else>
      <div class="muted">
        {{ llmReason(d.report.llm) }}{{ d.model ? ` · ${d.model}` : "" }}{{ d.tokens ? ` · ${d.tokens} tokens` : "" }}
        {{ d.report.attempts ? ` · 请求 ${d.report.attempts} 次` : "" }}
      </div>
      <h4>可能原因</h4>
      <div v-if="!d.report.causes.length" class="muted">没有 LLM 分析，只有统计结果：{{ d.report.summary }}</div>
      <ol v-else>
        <li v-for="(c, i) in d.report.causes" :key="i">
          {{ c.cause }} <span class="muted">（置信度 {{ num(c.confidence, 2) }}）</span>
          <ul><li v-for="(e, j) in c.evidence" :key="j" class="muted">{{ e }}</li></ul>
        </li>
      </ol>
      <template v-if="d.report.actions.length">
        <h4>建议操作（不会自动执行）</h4>
        <ul><li v-for="(a, i) in d.report.actions" :key="i">{{ a }}</li></ul>
      </template>
      <h4>触发异常</h4>
      <div>
        {{ d.anomaly.trigger.label }} · 规则 {{ d.anomaly.trigger.rule }}
        <template v-if="d.anomaly.trigger.z != null">
          · 当前 {{ num(d.anomaly.trigger.value, 4) }} · 中位数 {{ num(d.anomaly.trigger.median, 4) }} · z {{ num(d.anomaly.trigger.z, 1) }}
        </template>
        <template v-else-if="d.anomaly.trigger.value != null"> · 10 秒平均 {{ num(d.anomaly.trigger.value, 4) }}</template>
      </div>
      <div v-if="d.anomaly.related.length" class="muted">
        同时出现：{{ d.anomaly.related.map((r) => r.label).join("、") }}
      </div>
      <p><a href="#" class="muted" @click.prevent="showCtx = !showCtx">{{ showCtx ? "收起" : "查看" }}发给模型的上下文</a></p>
      <pre v-if="showCtx">{{ JSON.stringify(d.anomaly.context, null, 2) }}</pre>
    </template>
  </div>
</template>
