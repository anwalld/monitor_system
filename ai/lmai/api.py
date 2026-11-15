"""REST 接口：/api/ai/*。"""

from __future__ import annotations

import hmac
from typing import Any

from fastapi import Depends, FastAPI, HTTPException, Query, Request
from pydantic import BaseModel

from .diagnosis import Service
from .llm import Selection


class SettingsIn(BaseModel):
    vendor: str = ""
    model: str = ""
    api_key: str | None = None   # 非空：保存为该厂商的 Key（BYOK）；None：不改
    clear_key: bool = False      # 删掉看板里填的 Key（回落到 .env）


def key_hint(k: str) -> str:
    return f"…{k[-4:]}" if len(k) >= 8 else "已设置"


def create_app(svc: Service, lifespan: Any = None) -> FastAPI:
    app = FastAPI(title="lm-ai", lifespan=lifespan, docs_url=None, redoc_url=None, openapi_url=None)
    cfg = svc.cfg

    def auth(request: Request) -> None:
        if not cfg.api_token:
            return
        got = request.query_params.get("token", "")
        h = request.headers.get("authorization", "")
        if h.startswith("Bearer "):
            got = h[7:]
        if not hmac.compare_digest(got.encode(), cfg.api_token.encode()):
            raise HTTPException(401, "unauthorized")

    async def mysql_or_503(coro: Any) -> Any:
        try:
            return await coro
        except Exception as e:
            raise HTTPException(503, f"MySQL 不可用：{type(e).__name__}") from e

    @app.get("/api/ai/health")
    async def health() -> dict[str, Any]:
        st = await svc.llm.status()
        try:
            used = await svc.redis.used_today()
        except Exception:
            used = None
        return {
            "redis": await svc.redis.ping(), "mysql": await svc.mysql.ping(),
            "llm": {"enabled": st.enabled, "reason": st.reason,
                    "vendor": st.vendor.name if st.vendor else None, "model": st.model or None,
                    "last_call": svc.llm.last},
            "tokens_today": used, "budget": cfg.llm.daily_token_budget,
            "nodes": svc.det.node_count(), "samples": svc.samples_total, "warmed": svc.warmed,
            "queue": svc.queue.qsize(), "dropped": svc.dropped, "pending_rows": len(svc.pending_rows),
            "anomalies_total": svc.det.total, "diagnoses_total": svc.diagnoses_total,
        }

    @app.get("/api/ai/diagnoses", dependencies=[Depends(auth)])
    async def diagnoses(node: str | None = None, severity: str | None = None,
                        hours: float = Query(24, gt=0, le=24 * 90),
                        limit: int = Query(100, ge=1, le=1000)) -> dict[str, Any]:
        rows = await mysql_or_503(svc.mysql.list_diagnoses(node or None, severity or None, hours, limit))
        return {"diagnoses": rows}

    @app.get("/api/ai/diagnoses/{did}", dependencies=[Depends(auth)])
    async def diagnosis(did: int) -> dict[str, Any]:
        row = await mysql_or_503(svc.mysql.get_diagnosis(did))
        if row is None:
            raise HTTPException(404, "not found")
        return row

    @app.get("/api/ai/anomalies", dependencies=[Depends(auth)])
    async def anomalies(since_ms: int = 0, node: str | None = None) -> dict[str, Any]:
        out = [a.to_dict() for a in svc.det.recent if a.ts_ms >= since_ms and (not node or a.node == node)]
        return {"total": svc.det.total, "anomalies": out}

    def settings_view(sel: Selection, ui: dict[str, str]) -> dict[str, Any]:
        vendors = []
        for v in cfg.llm.vendors.values():
            uk, ek = ui.get(v.name, ""), v.api_key()
            src = "ui" if uk else "env" if ek else None
            vendors.append({"name": v.name, "label": v.label, "models": v.models, "key_configured": bool(src),
                            "key_source": src, "key_hint": key_hint(uk or ek) if src else None})
        return {"vendor": sel.vendor, "model": sel.model, "vendors": vendors}

    @app.get("/api/ai/settings", dependencies=[Depends(auth)])
    async def get_settings() -> dict[str, Any]:
        try:
            sel = await svc.redis.get_selection()
            ui = await svc.redis.ui_keys()
        except Exception as e:
            raise HTTPException(503, "Redis 不可用") from e
        st = await svc.llm.status()
        return {**settings_view(sel, ui), "enabled": st.enabled, "reason": st.reason}

    @app.put("/api/ai/settings", dependencies=[Depends(auth)])
    async def put_settings(body: SettingsIn) -> dict[str, Any]:
        sel = Selection(body.vendor.strip(), body.model.strip())
        if sel.vendor:
            v = cfg.llm.vendors.get(sel.vendor)
            if v is None:
                raise HTTPException(400, f"未知厂商 {sel.vendor}")
            if sel.model not in v.models:
                raise HTTPException(400, f"{v.label} 没有模型 {sel.model}")
        else:
            sel.model = ""
        new_key = (body.api_key or "").strip()
        if (new_key or body.clear_key) and not sel.vendor:
            raise HTTPException(400, "填写 Key 前先选择厂商")
        if new_key and (len(new_key) > 512 or any(c.isspace() for c in new_key)):
            raise HTTPException(400, "Key 格式不对")
        try:
            if new_key:
                await svc.redis.set_ui_key(sel.vendor, new_key)
            elif body.clear_key:
                await svc.redis.set_ui_key(sel.vendor, None)
            await svc.redis.set_selection(sel)
        except Exception as e:
            raise HTTPException(503, "Redis 不可用") from e
        return await get_settings()

    return app
