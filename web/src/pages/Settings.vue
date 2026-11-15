<script setup>
import { computed, onMounted, ref } from "vue";
import { api, aiDown } from "../live.js";
import { datetime, llmReason } from "../fmt.js";

const s = ref(null);
const health = ref(null);
const vendor = ref("");
const model = ref("");
const err = ref("");
const down = ref(false);
const saved = ref("");
const keyInput = ref("");

async function load() {
  try {
    s.value = await api("/api/ai/settings");
    vendor.value = s.value.vendor;
    model.value = s.value.model;
    health.value = await api("/api/ai/health");
    err.value = "";
    down.value = false;
  } catch (e) {
    down.value = aiDown(e);
    err.value = e.message;
  }
}
onMounted(load);

const current = computed(() => s.value?.vendors.find((v) => v.name === vendor.value));
function pickVendor(name) {
  vendor.value = name;
  const v = s.value.vendors.find((x) => x.name === name);
  model.value = v && v.models.includes(model.value) ? model.value : v?.models[0] || "";
}
async function save(clearKey = false) {
  saved.value = "";
  const body = { vendor: vendor.value, model: model.value };
  if (keyInput.value.trim()) body.api_key = keyInput.value.trim();
  if (clearKey) body.clear_key = true;
  try {
    s.value = await api("/api/ai/settings", { method: "PUT", body });
    keyInput.value = "";
    health.value = await api("/api/ai/health");
    saved.value = "已保存，下一次诊断生效";
    err.value = "";
  } catch (e) {
    down.value = aiDown(e);
    err.value = e.message;
  }
}
</script>

<template>
  <div v-if="down" class="notice">AI 服务不可用（{{ err }}）。调度和其他页面不受影响。</div>
  <div v-else-if="!s" class="empty">{{ err ? `加载失败：${err}` : "加载中…" }}</div>
  <div v-else class="card" style="max-width: 640px">
    <h3>LLM 厂商与模型</h3>
    <p class="muted">只发送指标和事件，不含任务命令和输出。API Key 可以在下面填写（保存在服务器的 Redis，之后不会再显示），也可以写在服务器的 deploy/.env；两处都有时用这里填的。不启用时诊断只有统计结果。</p>
    <div class="vendors">
      <label>
        <input type="radio" :checked="vendor === ''" @change="pickVendor('')" /> 不启用
      </label>
      <label v-for="v in s.vendors" :key="v.name">
        <input type="radio" :checked="vendor === v.name" @change="pickVendor(v.name)" />
        {{ v.label }}
        <span class="muted">{{ v.key_configured ? `Key 已配置（${v.key_source === "ui" ? "看板填写" : ".env"} ${v.key_hint}）` : "未配置 Key" }}</span>
      </label>
    </div>
    <p v-if="current">
      模型
      <select v-model="model">
        <option v-for="m in current.models" :key="m" :value="m">{{ m }}</option>
      </select>
    </p>
    <p v-if="current">
      API Key
      <input v-model="keyInput" type="password" autocomplete="off" size="36"
        :placeholder="current.key_configured ? '已配置，留空则不修改' : `填写 ${current.label} 的 API Key`" />
      <button v-if="current.key_source === 'ui'" @click="save(true)" style="margin-left: 6px">删除已填的 Key</button>
    </p>
    <p>
      <button @click="save()">保存</button>
      <span class="muted" style="margin-left: 10px">{{ saved }}{{ err ? `保存失败：${err}` : "" }}</span>
    </p>
    <p class="muted">
      当前状态：{{ s.enabled ? `已启用 ${s.vendor}/${s.model}` : llmReason(s.reason) }}
      <template v-if="health">
        · 今日 token {{ health.tokens_today ?? "?" }} / {{ health.budget }} · 检测节点 {{ health.nodes }} · 累计异常
        {{ health.anomalies_total }}，诊断 {{ health.diagnoses_total }}{{ health.mysql ? "" : " · MySQL 不可用" }}
      </template>
    </p>
    <p v-if="health?.llm?.last_call" class="muted" :style="{ color: health.llm.last_call.reason === 'ok' ? '' : 'var(--bad)' }">
      最近一次调用（{{ datetime(health.llm.last_call.ts_ms) }}，{{ health.llm.last_call.model }}）：
      {{ health.llm.last_call.reason === "ok" ? `成功，${health.llm.last_call.tokens} tokens` : llmReason(health.llm.last_call.reason) }}
    </p>
  </div>
</template>
