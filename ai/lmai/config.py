"""配置：config/ai.yaml + 环境变量覆盖（密码、Key 只从环境变量读）。"""

from __future__ import annotations

import os
from dataclasses import dataclass, field
from typing import Any

import yaml


@dataclass
class MetricCfg:
    min_scale: float
    direction: str  # up / down


@dataclass
class DetectCfg:
    window_s: float = 600
    min_samples: int = 60
    z_threshold: float = 4.0
    z_high: float = 10.0
    consecutive: int = 3
    metrics: dict[str, MetricCfg] = field(default_factory=dict)
    runq_drop_rate_max: float = 0.01
    nic_timeout_max: float = 0.20
    threshold_avg_s: float = 10
    event_kinds: list[str] = field(default_factory=lambda: ["evict", "rescue_start"])
    event_max_age_s: float = 600
    dedup_s: float = 600
    related_s: float = 60


@dataclass
class DiagnosisCfg:
    queue_max: int = 32
    settle_s: float = 5          # 检出后等几秒再组装上下文，把紧随其后的异常（如剔除）一起带上
    recent_step_s: int = 10
    events_lookback_s: int = 1800
    anonymize: bool = False


@dataclass
class VendorCfg:
    name: str
    label: str
    base_url: str
    key_env: str
    models: list[str]
    json_mode: bool = True

    def api_key(self) -> str:
        return os.environ.get(self.key_env, "").strip()


@dataclass
class LlmCfg:
    timeout_s: float = 30
    max_retries: int = 2
    daily_token_budget: int = 1000000
    max_tokens: int = 1200
    temperature: float = 0.2
    vendors: dict[str, VendorCfg] = field(default_factory=dict)
    backoff_s: float = 1.0  # 第 n 次重试前等 backoff_s × 2^(n-1)


@dataclass
class ChatCfg:
    max_tool_calls: int = 6          # 一轮问答最多调几次工具
    timeout_s: float = 120           # 一轮问答总时长上限
    history_max: int = 10            # 只取最近几轮对话
    max_tokens: int = 1500
    result_max_bytes: int = 8000     # 单个工具结果的 JSON 上限，超出截断序列
    # 与 manager.yaml 的 scoring.*.weight 保持一致（Manager 不把权重写进 Redis）
    score_weights: dict[str, float] = field(
        default_factory=lambda: {"cpu": 0.3, "runq": 0.3, "mem": 0.2, "net": 0.2})


@dataclass
class PredictCfg:
    enabled: bool = True             # 计算预测与在线评估
    push: bool = False               # 推给 Manager（enabled 也为 true 时才推）；M9 验收未优于朴素基线，默认关闭
    interval_s: float = 5
    horizon_s: float = 30
    alpha: float = 0.3
    beta: float = 0.05
    phi: float = 0.9                 # 阻尼趋势：趋势每秒乘 φ（1.0 为普通 Holt）
    min_samples: int = 30
    ttl_ms: int = 10000
    fresh_s: float = 10              # 最后样本超过该秒数的节点不推
    factor_knee: float = 0.7         # 预测值不超过它时因子为 1
    factor_min: float = 0.5          # 预测值达到 1.0 时的因子
    eval_window_s: float = 3600      # 在线 MAE 窗口
    manager_addr: str = "127.0.0.1:50051"
    rpc_timeout_s: float = 2


@dataclass
class ReportCfg:
    enabled: bool = True
    at: str = "09:00"                # 每天几点（服务器本地时间）生成前一天的日报
    max_tokens: int = 1500


@dataclass
class Config:
    listen_host: str = "0.0.0.0"
    listen_port: int = 8000
    log_level: str = "info"
    redis_uri: str = "tcp://127.0.0.1:6379"
    redis_password: str = ""
    redis_group: str = "ai"
    redis_consumer: str = "ai-1"
    key_prefix: str = ""
    mysql_host: str = "127.0.0.1"
    mysql_port: int = 3306
    mysql_user: str = "lm"
    mysql_database: str = "lm"
    mysql_password: str = ""
    api_token: str = ""
    detect: DetectCfg = field(default_factory=DetectCfg)
    diagnosis: DiagnosisCfg = field(default_factory=DiagnosisCfg)
    llm: LlmCfg = field(default_factory=LlmCfg)
    chat: ChatCfg = field(default_factory=ChatCfg)
    predict: PredictCfg = field(default_factory=PredictCfg)
    report: ReportCfg = field(default_factory=ReportCfg)


DEFAULT_METRICS = {
    "cpu": MetricCfg(0.05, "up"),
    "runq_p99_us": MetricCfg(100, "up"),
    "mem_avail": MetricCfg(0.02, "down"),
    "disk": MetricCfg(0.05, "up"),
    "rtt_ms": MetricCfg(2.0, "up"),
    "timeout_rate": MetricCfg(0.05, "up"),
    "retrans_rate": MetricCfg(0.01, "up"),
}


def _fill(obj: Any, d: dict[str, Any], skip: tuple[str, ...] = ()) -> None:
    for k, v in (d or {}).items():
        if k in skip or not hasattr(obj, k):
            continue
        cur = getattr(obj, k)
        if isinstance(cur, bool):
            v = bool(v)
        elif isinstance(cur, int) and not isinstance(cur, bool):
            v = int(v)
        elif isinstance(cur, float):
            v = float(v)
        setattr(obj, k, v)


def from_dict(raw: dict[str, Any], env: dict[str, str] | None = None) -> Config:
    env = dict(os.environ if env is None else env)
    c = Config()
    listen = str(raw.get("listen", "0.0.0.0:8000"))
    host, _, port = listen.rpartition(":")
    c.listen_host, c.listen_port = host or "0.0.0.0", int(port)
    c.log_level = str(raw.get("log_level", c.log_level))
    r = raw.get("redis") or {}
    c.redis_uri = str(r.get("uri", c.redis_uri))
    c.redis_group = str(r.get("group", c.redis_group))
    c.redis_consumer = str(r.get("consumer", c.redis_consumer))
    m = raw.get("mysql") or {}
    c.mysql_host = str(m.get("host", c.mysql_host))
    c.mysql_port = int(m.get("port", c.mysql_port))
    c.mysql_user = str(m.get("user", c.mysql_user))
    c.mysql_database = str(m.get("database", c.mysql_database))

    d = raw.get("detect") or {}
    _fill(c.detect, d, skip=("metrics",))
    c.detect.metrics = dict(DEFAULT_METRICS)
    for name, mc in (d.get("metrics") or {}).items():
        c.detect.metrics[name] = MetricCfg(float(mc["min_scale"]), str(mc.get("direction", "up")))
    _fill(c.diagnosis, raw.get("diagnosis") or {})
    l = raw.get("llm") or {}
    _fill(c.llm, l, skip=("vendors",))
    for name, v in (l.get("vendors") or {}).items():
        c.llm.vendors[name] = VendorCfg(
            name=name,
            label=str(v.get("label", name)),
            base_url=str(v["base_url"]),
            key_env=str(v.get("key_env", f"{name.upper()}_API_KEY")),
            models=[str(x) for x in v.get("models", [])],
            json_mode=bool(v.get("json_mode", True)),
        )

    ch = raw.get("chat") or {}
    _fill(c.chat, ch, skip=("score_weights",))
    if ch.get("score_weights"):
        c.chat.score_weights = {str(k): float(v) for k, v in ch["score_weights"].items()}
    _fill(c.predict, raw.get("predict") or {})
    _fill(c.report, raw.get("report") or {})
    c.report.at = str(c.report.at)

    # 环境变量覆盖
    c.predict.manager_addr = env.get("LM_MANAGER_ADDR", c.predict.manager_addr)
    c.redis_uri = env.get("LM_REDIS_URI", c.redis_uri)
    c.redis_password = env.get("LM_REDIS_PASSWORD", c.redis_password)
    c.key_prefix = env.get("LM_AI_KEY_PREFIX", c.key_prefix)
    c.mysql_host = env.get("LM_MYSQL_HOST", c.mysql_host)
    c.mysql_port = int(env.get("LM_MYSQL_PORT", c.mysql_port))
    c.mysql_user = env.get("LM_MYSQL_USER", c.mysql_user)
    c.mysql_database = env.get("LM_MYSQL_DATABASE", c.mysql_database)
    c.mysql_password = env.get("LM_MYSQL_PASSWORD", c.mysql_password)
    c.api_token = env.get("LM_API_TOKEN", c.api_token)
    if "LM_AI_LISTEN" in env:
        host, _, port = env["LM_AI_LISTEN"].rpartition(":")
        c.listen_host, c.listen_port = host or "0.0.0.0", int(port)
    return c


def load(path: str) -> Config:
    with open(path, encoding="utf-8") as f:
        return from_dict(yaml.safe_load(f) or {})
