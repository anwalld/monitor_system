// 与 Manager /ws 的唯一连接：snapshot / node_update 更新节点，event 按 id 去重保存。
// 断开后按 1、2、4、8、16、30 秒退避重连（之后保持 30 秒，连上后归零），重连带 since=<最大事件 id>。
import { reactive } from "vue";

const MAX_EVENTS = 2000;

export const live = reactive({
  status: "connecting", // connecting / open / closed / unauthorized
  connects: 0,
  retryIn: 0,
  nodes: [],
  nodesTs: 0,
  events: [], // 按 id 倒序
  lastEventId: "",
  token: loadToken(),
});

function loadToken() {
  const m = location.hash.match(/[?&]token=([^&]+)/);
  if (m) {
    try { localStorage.setItem("lm_token", decodeURIComponent(m[1])); } catch {}
    return decodeURIComponent(m[1]);
  }
  try { return localStorage.getItem("lm_token") || ""; } catch { return ""; }
}

export function setToken(t) {
  live.token = t;
  try { localStorage.setItem("lm_token", t); } catch {}
  reconnectNow();
}

// Stream id "ms-seq" 比较
export function idLess(a, b) {
  const [am, as] = a.split("-").map(Number);
  const [bm, bs] = b.split("-").map(Number);
  return am < bm || (am === bm && as < bs);
}

const seen = new Map(); // id → event
let ws = null;
let attempt = 0;
let timer = null;
const listeners = new Set();

export function onEvent(fn) {
  listeners.add(fn);
  return () => listeners.delete(fn);
}

export function addEvent(e) {
  if (seen.has(e.id)) return false;
  seen.set(e.id, e);
  // 插入到倒序位置（实时推送通常在最前）
  let i = 0;
  while (i < live.events.length && idLess(e.id, live.events[i].id)) i++;
  live.events.splice(i, 0, e);
  if (live.events.length > MAX_EVENTS) {
    const old = live.events.pop();
    seen.delete(old.id);
  }
  if (!live.lastEventId || idLess(live.lastEventId, e.id)) live.lastEventId = e.id;
  return true;
}

function url() {
  const q = new URLSearchParams();
  if (live.token) q.set("token", live.token);
  if (live.lastEventId) q.set("since", live.lastEventId);
  const proto = location.protocol === "https:" ? "wss:" : "ws:";
  return `${proto}//${location.host}/ws${q.toString() ? "?" + q : ""}`;
}

export function connect() {
  clearTimeout(timer);
  live.status = "connecting";
  const sock = new WebSocket(url());
  ws = sock;
  sock.onopen = () => {
    attempt = 0;
    live.connects++;
    live.status = "open";
  };
  sock.onmessage = (m) => {
    const msg = JSON.parse(m.data);
    if (msg.type === "snapshot" || msg.type === "node_update") {
      live.nodes = msg.nodes;
      live.nodesTs = msg.ts;
      if (msg.type === "snapshot" && !live.lastEventId && msg.last_event_id) {
        live.lastEventId = msg.last_event_id;
      }
    } else if (msg.type === "event") {
      if (addEvent(msg)) listeners.forEach((fn) => fn(msg));
    }
  };
  sock.onclose = (e) => {
    if (ws !== sock) return;
    ws = null;
    // 1008：token 不对（服务端升级后立即以 policy violation 关闭），等用户输入新 token
    if (e.code === 1008) {
      live.status = "unauthorized";
      return;
    }
    live.status = "closed";
    const delay = Math.min(30, 2 ** attempt++);
    live.retryIn = delay;
    timer = setTimeout(connect, delay * 1000);
  };
  sock.onerror = () => {};
}

export function reconnectNow() {
  attempt = 0;
  if (ws) {
    const s = ws;
    ws = null;
    s.close();
  }
  connect();
}

// REST
export async function api(path) {
  const r = await fetch(path, { headers: live.token ? { Authorization: `Bearer ${live.token}` } : {} });
  if (r.status === 401) {
    live.status = "unauthorized";
    throw new Error("unauthorized");
  }
  if (!r.ok) throw new Error(`${r.status}`);
  return r.json();
}
