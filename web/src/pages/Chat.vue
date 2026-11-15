<script setup>
// 运维问答（M9）：POST /api/ai/chat，SSE 流式显示回答和工具调用。EventSource 不支持 POST，用 fetch + ReadableStream 解析。
import { nextTick, ref } from "vue";
import { live } from "../live.js";
import { llmReason } from "../fmt.js";

const msgs = ref([]); // { role, content, tools: [{id,name,args,ok,ms,summary}], done, error }
const input = ref("");
const busy = ref(false);
const listEl = ref(null);
const examples = ["现在哪台节点最闲？", "lm-worker-1 为什么分数低？", "最近 1 小时任务完成时间 P99 是多少？", "最近 24 小时有哪些 AI 诊断？"];
let controller = null;

const TOOL_LABEL = {
  list_nodes: "列出节点", get_node_metrics: "查询指标", get_node_events: "查询事件", explain_score: "解释分数",
  get_task_stats: "任务统计", get_diagnoses: "查询诊断",
};

async function scroll() {
  await nextTick();
  listEl.value?.scrollTo({ top: listEl.value.scrollHeight });
}

function history() {
  // 最近 10 轮：只发文本，不发工具细节
  return msgs.value.filter((m) => m.content && !m.error).slice(-20).map((m) => ({ role: m.role, content: m.content }));
}

async function send(text) {
  const q = (text ?? input.value).trim();
  if (!q || busy.value) return;
  input.value = "";
  msgs.value.push({ role: "user", content: q });
  const reply = { role: "assistant", content: "", pre: "", tools: [], done: null, error: "" };
  msgs.value.push(reply);
  const r = msgs.value[msgs.value.length - 1]; // 响应式代理
  busy.value = true;
  scroll();
  controller = new AbortController();
  try {
    const headers = { "Content-Type": "application/json" };
    if (live.token) headers.Authorization = `Bearer ${live.token}`;
    const resp = await fetch("/api/ai/chat", {
      method: "POST", headers, body: JSON.stringify({ messages: history() }),
      signal: controller.signal,
    });
    if (resp.status === 401) {
      live.status = "unauthorized";
      throw new Error("token 无效");
    }
    if (!resp.ok) {
      let detail = "";
      try { detail = (await resp.json()).detail || ""; } catch {}
      throw Object.assign(new Error(detail || `HTTP ${resp.status}`), { status: resp.status });
    }
    const reader = resp.body.getReader();
    const dec = new TextDecoder();
    let buf = "";
    for (;;) {
      const { value, done } = await reader.read();
      if (done) break;
      buf += dec.decode(value, { stream: true });
      let i;
      while ((i = buf.indexOf("\n\n")) >= 0) {
        const block = buf.slice(0, i);
        buf = buf.slice(i + 2);
        let ev = "message", data = "";
        for (const line of block.split("\n")) {
          if (line.startsWith("event: ")) ev = line.slice(7);
          else if (line.startsWith("data: ")) data += line.slice(6);
        }
        if (!data) continue;
        const d = JSON.parse(data);
        if (ev === "delta") r.content += d.text;
        else if (ev === "tool_call") {
          // 模型调工具前说的话（“我先查一下……”）不算回答正文，单独放
          if (r.content.trim()) r.pre += (r.pre ? " " : "") + r.content.trim();
          r.content = "";
          r.tools.push({ id: d.id, name: d.name, args: d.args, pending: true });
        }
        else if (ev === "tool_result") {
          const t = r.tools.find((x) => x.id === d.id && x.pending);
          if (t) Object.assign(t, { pending: false, ok: d.ok, ms: d.ms, summary: d.summary });
        } else if (ev === "done") r.done = d;
        else if (ev === "error") r.error = d.reason;
        scroll();
      }
    }
  } catch (e) {
    if (e.name !== "AbortError") {
      r.error = [0, 502, 503, 504].includes(e.status) || e instanceof TypeError ? "AI 服务不可用" : e.message;
    }
  }
  busy.value = false;
  controller = null;
  scroll();
}

function stop() {
  controller?.abort();
}
function clear() {
  if (!busy.value) msgs.value = [];
}
const errText = (r) =>
  r === "disabled" || r === "no_key" || r === "unknown_vendor" || r === "unknown_model"
    ? `${llmReason(r)}：请到“设置”页选择厂商和模型并填写 Key`
    : llmReason(r);
const argText = (a) => (a && typeof a === "object" ? Object.entries(a).map(([k, v]) => `${k}=${v}`).join(", ") : a || "");
</script>

<template>
  <div class="chat card">
    <div ref="listEl" class="msgs">
      <div v-if="!msgs.length" class="empty">
        <p>用自然语言询问节点状态、分数、事件、任务和诊断。助手只能读取数据，不会执行任何操作。</p>
        <p><button v-for="e in examples" :key="e" class="chip" @click="send(e)">{{ e }}</button></p>
      </div>
      <div v-for="(m, i) in msgs" :key="i" class="msg" :class="m.role">
        <template v-if="m.role === 'user'"><div class="bubble">{{ m.content }}</div></template>
        <template v-else>
          <div v-if="m.pre" class="muted pre">{{ m.pre }}</div>
          <details v-if="m.tools.length" class="tools" :open="busy && i === msgs.length - 1">
            <summary class="muted">调用了 {{ m.tools.length }} 个工具</summary>
            <div v-for="t in m.tools" :key="t.id" class="tool muted">
              <span :class="t.pending ? '' : t.ok ? 'ok' : 'bad'">{{ t.pending ? "…" : t.ok ? "✓" : "✗" }}</span>
              {{ TOOL_LABEL[t.name] || t.name }}<code>{{ t.name }}({{ argText(t.args) }})</code>
              <template v-if="!t.pending"> → {{ t.summary }}（{{ t.ms }} ms）</template>
            </div>
          </details>
          <div class="bubble answer">{{ m.content }}<span v-if="busy && i === msgs.length - 1 && !m.error" class="cursor">▍</span></div>
          <div v-if="m.error" class="notice">{{ errText(m.error) }}</div>
          <div v-if="m.done" class="muted">{{ m.done.model }} · {{ m.done.tokens }} tokens · {{ (m.done.ms / 1000).toFixed(1) }} 秒</div>
        </template>
      </div>
    </div>
    <form class="inputbar" @submit.prevent="send()">
      <input v-model="input" placeholder="例如：lm-worker-1 最近 1 小时被剔除过吗？" :disabled="busy" />
      <button v-if="busy" type="button" @click="stop">停止</button>
      <button v-else type="submit" :disabled="!input.trim()">发送</button>
      <button type="button" :disabled="busy || !msgs.length" @click="clear">清空</button>
    </form>
  </div>
</template>

<style scoped>
.chat { display: flex; flex-direction: column; height: calc(100vh - 110px); min-height: 420px; padding: 0; }
.msgs { flex: 1; overflow-y: auto; padding: 14px 16px; }
.msg { margin: 10px 0; display: flex; flex-direction: column; gap: 4px; }
.msg.user { align-items: flex-end; }
.bubble { max-width: 80%; padding: 8px 12px; border-radius: 10px; white-space: pre-wrap; line-height: 1.6; }
.user .bubble { background: var(--info); color: #fff; }
.answer { background: var(--bg); max-width: 92%; }
.tools { font-size: 12px; max-width: 92%; }
.pre { max-width: 92%; font-style: italic; }
.tool { padding: 2px 0 2px 12px; }
.tool code { margin: 0 4px; font-size: 11px; word-break: break-all; }
.ok { color: var(--ok); } .bad { color: var(--bad); }
.cursor { animation: blink 1s step-end infinite; }
@keyframes blink { 50% { opacity: 0; } }
.inputbar { display: flex; gap: 8px; padding: 10px 12px; border-top: 1px solid var(--line); }
.inputbar input { flex: 1; min-width: 0; }
.chip { margin: 4px; border-radius: 99px; font-size: 12px; }
</style>
