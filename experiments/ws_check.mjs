#!/usr/bin/env node
// 看板断线重连验收：连上 /ws → 主动断开（或等服务端断开）→ 期间执行 --during 命令制造事件 →
// 按 1、2、4…30 秒退避带 since 重连 → 与 GET /api/events 对比，检查事件不丢不重。
// 需要 Node 22+（自带 WebSocket）。
//
//   node experiments/ws_check.mjs --url http://172.16.255.133:8080 [--token T] \
//        [--close-after 5] [--during "ssh … docker pause lmtest-worker2"] [--gap 15] [--settle 8]
//
// --close-after 秒后客户端主动断开；设为 0 时不主动断开（用于验证服务端重启后的自动重连）。
// --during 在断开后执行（shell），执行完再等 --gap 秒才重连；--settle 为重连后继续收集的秒数。
// 退出码：0 通过，1 有遗漏或重复。

import { execSync } from "node:child_process";

const args = Object.fromEntries(
  process.argv.slice(2).reduce((acc, a, i, all) => {
    if (a.startsWith("--")) acc.push([a.slice(2), all[i + 1] && !all[i + 1].startsWith("--") ? all[i + 1] : "1"]);
    return acc;
  }, []),
);
const base = (args.url || "http://127.0.0.1:8080").replace(/\/$/, "");
const token = args.token || "";
const closeAfter = Number(args["close-after"] ?? 5);
const gap = Number(args.gap ?? 15);
const settle = Number(args.settle ?? 8);
const total = Number(args.total ?? 0); // 0 表示按阶段结束

const idKey = (id) => id.split("-").map(BigInt);
const idLess = (a, b) => {
  const [am, as] = idKey(a), [bm, bs] = idKey(b);
  return am < bm || (am === bm && as < bs);
};
const log = (...m) => console.log(new Date().toISOString().slice(11, 23), ...m);

const events = new Map(); // id → event（去重后）
let raw = 0; // 收到的 event 消息总数（含补发与实时的重叠）
let firstBaseline = null; // 第一次 snapshot 的 last_event_id：只比较它之后的事件
let lastId = "";
let connects = 0;
let snapshots = 0;
let ws = null;
let phase = "first"; // first → closed → reconnected
let attempt = 0;

function connect() {
  const q = new URLSearchParams();
  if (token) q.set("token", token);
  if (lastId) q.set("since", lastId);
  const url = base.replace(/^http/, "ws") + "/ws" + (q.toString() ? "?" + q : "");
  ws = new WebSocket(url);
  ws.onopen = () => {
    connects++;
    attempt = 0;
    log(`已连接（第 ${connects} 次）since=${lastId || "-"}`);
    if (connects > 1) phase = "reconnected";
  };
  ws.onmessage = (m) => {
    const msg = JSON.parse(m.data);
    if (msg.type === "event") {
      raw++;
      if (!events.has(msg.id)) events.set(msg.id, msg);
      if (!lastId || idLess(lastId, msg.id)) lastId = msg.id;
      log(`event ${msg.id} ${msg.node} ${msg.kind}`);
    } else if (msg.type === "snapshot") {
      snapshots++;
      if (firstBaseline === null) firstBaseline = msg.last_event_id || "0-0";
      if (!lastId && msg.last_event_id) lastId = msg.last_event_id;
      log(`snapshot：${msg.nodes.length} 个节点，last_event_id=${msg.last_event_id || "-"}`);
    }
  };
  ws.onclose = (e) => {
    log(`连接关闭 code=${e.code}`);
    ws = null;
    if (phase === "done") return;
    if (phase === "first" && closeAfter > 0) return; // 主动断开：由主流程安排重连
    const delay = Math.min(30, 2 ** attempt++);
    log(`${delay} 秒后重连`);
    setTimeout(connect, delay * 1000);
  };
  ws.onerror = () => {};
}

const sleep = (s) => new Promise((r) => setTimeout(r, s * 1000));

async function main() {
  connect();
  if (closeAfter > 0) {
    await sleep(closeAfter);
    log("主动断开");
    ws?.close();
    await sleep(0.5);
    phase = "closed";
    if (args.during) {
      log(`执行：${args.during}`);
      try {
        execSync(args.during, { stdio: "inherit", shell: "/bin/bash" });
      } catch (e) {
        log(`命令失败：${e.message}`);
      }
    }
    await sleep(gap);
    connect();
    await sleep(settle);
  } else {
    if (args.during) {
      await sleep(3);
      log(`执行：${args.during}`);
      try {
        execSync(args.during, { stdio: "inherit", shell: "/bin/bash" });
      } catch (e) {
        log(`命令失败：${e.message}`);
      }
    }
    await sleep(total || gap + settle);
  }
  phase = "done";
  ws?.close();

  const headers = token ? { Authorization: `Bearer ${token}` } : {};
  const r = await fetch(`${base}/api/events?limit=1000`, { headers });
  const server = (await r.json()).events.filter((e) => idLess(firstBaseline, e.id)).map((e) => e.id);
  const client = [...events.keys()].filter((id) => idLess(firstBaseline, id));
  const missing = server.filter((id) => !events.has(id));
  const extra = client.filter((id) => !server.includes(id));
  console.log(
    JSON.stringify(
      {
        baseline: firstBaseline,
        connects,
        snapshots,
        server_events: server.length,
        client_unique: client.length,
        raw_event_messages: raw,
        overlap_deduped: raw - events.size,
        missing,
        extra,
      },
      null,
      1,
    ),
  );
  const ok = missing.length === 0 && extra.length === 0 && connects >= 2;
  console.log(ok ? "通过：不丢不重" : "失败");
  process.exit(ok ? 0 : 1);
}

main();
