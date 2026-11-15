"""负载预测（M9）：Holt、因子换算、在线 MAE、推送批内容、Advisor gRPC 往返。"""

import asyncio

import pytest

from fakes import FakeMySQL, FakeRedis
from lmai.detector import Detector
from lmai.diagnosis import Service
from lmai.llm import LlmClient
from lmai.predict import Holt, Predictor, Pusher, factor


def test_factor_table():
    assert factor(0.5) == 1.0 and factor(0.7) == 1.0
    assert factor(0.85) == pytest.approx(0.75)
    assert factor(1.0) == 0.5 and factor(1.2) == 0.5
    assert all(factor(x / 100) <= 1.0 for x in range(0, 120))


def test_holt_constant_and_ramp():
    h = Holt(0.3, 0.1)
    for i in range(100):
        h.update(i * 1000, 0.4)
    assert h.forecast(30) == pytest.approx(0.4)
    h = Holt(0.3, 0.1)
    for i in range(60):
        h.update(i * 1000, 0.1 + 0.01 * i)  # 每秒 +0.01
    assert h.forecast(30) > 0.69 and h.forecast(30) == pytest.approx(0.69 + 0.3, abs=0.06)
    assert Holt().forecast(30) == 0.0
    h.update(59_000, 5.0)  # 重复时间戳忽略
    assert h.n == 60
    # 阻尼趋势：外推只加 Σφ^i（φ 0.9 时约 8.6 秒）的趋势，仍高于当前值但远小于线性外推
    d = Holt(0.3, 0.05, 0.9)
    for i in range(60):
        d.update(i * 1000, 0.1 + 0.01 * i)
    assert 0.69 < d.forecast(30) < 0.69 + 0.01 * 10
    assert Holt(0.3, 0.05, 0.9).forecast(30) == 0.0


def test_predictor_ramp_beats_naive_and_tick_rules():
    p = Predictor(min_samples=30, fresh_s=10)
    ts = 0
    for i in range(300):
        ts = i * 1000
        p.observe("w1", ts, min(1.0, 0.05 + 0.002 * i))  # 缓升
        p.observe("idle", ts, 0.02)
        if i % 5 == 4:
            p.tick(ts)
    v = p.view(ts)
    w1 = v["nodes"]["w1"]
    assert w1["points"] > 40 and w1["mae_holt"] < w1["mae_naive"]
    assert v["nodes"]["idle"]["mae_holt"] == pytest.approx(0, abs=1e-9)
    # 样本不足、不新鲜的节点不输出
    p.observe("new", ts, 0.5)
    out = {i["node_id"] for i in p.tick(ts)}
    assert out == {"w1", "idle"}
    assert p.tick(ts + 11_000) == []
    p.observe("w1", ts - 5000, 0.9)  # 乱序样本忽略
    assert p.nodes["w1"].last_ts == ts


def test_predictor_window_trims_errors():
    p = Predictor(min_samples=1, eval_window_s=60)
    for i in range(200):
        p.observe("w", i * 1000, 0.3)
        p.tick(i * 1000)
    assert p.view(199_000)["nodes"]["w"]["points"] <= 61


class FakePusher:
    def __init__(self, fail=False):
        self.batches, self.fail = [], fail

    async def push(self, items, ts_ms, ttl_ms):
        if self.fail:
            raise ConnectionError("manager down")
        self.batches.append((items, ts_ms, ttl_ms))

    async def close(self):
        pass


def make_svc(cfg, now_s):
    r = FakeRedis()
    llm = LlmClient(cfg.llm, r.get_selection, r)
    svc = Service(cfg, r, FakeMySQL(), llm, Detector(cfg.detect), clock=lambda: now_s[0])
    return svc, r


async def test_service_predict_once_pushes_and_writes_redis(cfg):
    now = [1000.0]
    cfg.predict.push = True  # 正式配置默认关闭推送，这里打开测推送路径
    svc, r = make_svc(cfg, now)
    svc.pusher = FakePusher()
    for i in range(40):
        svc.on_metrics_entry({"nodes": f'[{{"node_id":"w1","ts_ms":"{(960 + i) * 1000}","cpu_busy":0.9}}]'})
    items = await svc.predict_once()
    assert [i["node_id"] for i in items] == ["w1"]
    (batch, ts, ttl), = svc.pusher.batches
    assert ttl == 10000 and ts == 1_000_000 and batch[0]["factor"] == pytest.approx(factor(0.9), abs=1e-4)
    preds, ttl_s = r.set_preds_calls[0]
    assert ttl_s == 10 and preds["w1"]["cpu_busy_30s"] == pytest.approx(0.9, abs=1e-4)
    h = svc.predict_health()
    assert h["push_ok"] == 1 and h["nodes"] == 1 and h["push"]
    # 推送失败只计数
    svc.pusher = FakePusher(fail=True)
    await svc.predict_once()
    assert svc.predict_health()["push_fail"] == 1
    # push 关掉时不推，但仍写 ai:pred
    cfg.predict.push = False
    svc.pusher = FakePusher()
    await svc.predict_once()
    assert svc.pusher.batches == [] and len(r.set_preds_calls) == 3


async def test_pusher_grpc_roundtrip():
    grpc = pytest.importorskip("grpc")
    try:
        from lmai.pb import advisor_pb2_grpc, monitor_pb2
    except ImportError:
        pytest.skip("没有生成 lmai/pb")
    got = []

    class Svc(advisor_pb2_grpc.AdvisorServicer):
        async def PushPredictions(self, request, context):
            got.append(request)
            return monitor_pb2.Ack()

    server = grpc.aio.server()
    advisor_pb2_grpc.add_AdvisorServicer_to_server(Svc(), server)
    port = server.add_insecure_port("127.0.0.1:0")
    await server.start()
    try:
        p = Pusher(f"127.0.0.1:{port}", 2)
        await p.push([{"node_id": "w1", "cpu_busy_30s": 0.85, "factor": 0.75}], 123, 10000)
        await p.close()
    finally:
        await server.stop(0)
    assert len(got) == 1 and got[0].ttl_ms == 10000 and got[0].ts_ms == 123
    assert got[0].items[0].node_id == "w1" and got[0].items[0].factor == pytest.approx(0.75)
    with pytest.raises(Exception):
        await asyncio.wait_for(Pusher("127.0.0.1:1", 0.3).push([], 0, 1), 5)
