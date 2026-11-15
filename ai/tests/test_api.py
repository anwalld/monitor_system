"""REST 接口：设置校验、BYOK 不回显、鉴权、health（用假的 Redis/MySQL，不需要外部服务）。"""

from fastapi.testclient import TestClient

from lmai.api import create_app
from lmai.detector import Detector
from lmai.diagnosis import Service
from lmai.llm import LlmClient, Selection


class FakeRedis:
    def __init__(self):
        self.settings, self.keys, self.tokens = {}, {}, 0

    async def ping(self):
        return True

    async def get_selection(self):
        v = self.settings.get("vendor", "")
        return Selection(v, self.settings.get("model", ""), self.keys.get(v, ""))

    async def set_selection(self, sel):
        self.settings = {"vendor": sel.vendor, "model": sel.model}

    async def ui_keys(self):
        return dict(self.keys)

    async def set_ui_key(self, vendor, key):
        if key:
            self.keys[vendor] = key
        else:
            self.keys.pop(vendor, None)

    async def used_today(self):
        return self.tokens

    async def add(self, n):
        self.tokens += n


class DownMySQL:
    async def ping(self):
        return False

    async def list_diagnoses(self, *a):
        raise ConnectionError("down")


def client(cfg, monkeypatch, token=""):
    monkeypatch.delenv("DEEPSEEK_API_KEY", raising=False)
    cfg.api_token = token
    r = FakeRedis()
    svc = Service(cfg, r, DownMySQL(), LlmClient(cfg.llm, r.get_selection, r), Detector(cfg.detect))
    return TestClient(create_app(svc)), r


def test_settings_validation_and_byok(cfg, monkeypatch):
    c, r = client(cfg, monkeypatch)
    assert c.put("/api/ai/settings", json={"vendor": "deepseek", "model": "gpt-4"}).status_code == 400
    assert c.put("/api/ai/settings", json={"vendor": "nope", "model": "x"}).status_code == 400
    assert r.settings == {}                                   # 400 时不改
    assert c.put("/api/ai/settings", json={"vendor": "", "api_key": "sk-x"}).status_code == 400
    d = c.put("/api/ai/settings", json={"vendor": "deepseek", "model": "deepseek-chat"}).json()
    assert d["reason"] == "no_key" and not d["enabled"]
    d = c.put("/api/ai/settings", json={"vendor": "deepseek", "model": "deepseek-chat",
                                        "api_key": "sk-secret-abcd1234"}).json()
    ds = next(v for v in d["vendors"] if v["name"] == "deepseek")
    assert d["enabled"] and ds["key_source"] == "ui" and ds["key_hint"] == "…1234"
    assert "sk-secret" not in c.get("/api/ai/settings").text  # Key 不回显
    d = c.put("/api/ai/settings", json={"vendor": "deepseek", "model": "deepseek-chat", "clear_key": True}).json()
    assert d["reason"] == "no_key" and "deepseek" not in r.keys


def test_health_with_mysql_down_and_auth(cfg, monkeypatch):
    c, _ = client(cfg, monkeypatch, token="tk")
    h = c.get("/api/ai/health")                                # 不鉴权
    assert h.status_code == 200 and h.json()["mysql"] is False and h.json()["redis"] is True
    assert c.get("/api/ai/settings").status_code == 401
    assert c.get("/api/ai/settings", headers={"Authorization": "Bearer tk"}).status_code == 200
    assert c.get("/api/ai/diagnoses?token=tk").status_code == 503   # MySQL 不可用
    assert c.get("/api/ai/anomalies?token=tk").json() == {"total": 0, "anomalies": []}
