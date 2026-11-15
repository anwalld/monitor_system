"""入口：python -m lmai.main [-c config/ai.yaml]"""

from __future__ import annotations

import argparse
import logging
import os
import sys
from contextlib import asynccontextmanager

import uvicorn

from . import config as config_mod
from .api import create_app
from .diagnosis import Service
from .llm import LlmClient
from .store import MySQL, Redis


def build(cfg: config_mod.Config) -> Service:
    redis = Redis(cfg)
    mysql = MySQL(cfg)
    llm = LlmClient(cfg.llm, redis.get_selection, redis)
    return Service(cfg, redis, mysql, llm)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="lm-ai")
    ap.add_argument("-c", "--config", default=os.environ.get("LM_AI_CONFIG", "config/ai.yaml"))
    args = ap.parse_args(argv)
    cfg = config_mod.load(args.config)
    logging.basicConfig(level=cfg.log_level.upper(), stream=sys.stderr,
                        format="%(asctime)s %(levelname)s %(name)s: %(message)s")
    logging.getLogger("httpx").setLevel(logging.WARNING)
    svc = build(cfg)
    if not cfg.api_token:
        logging.getLogger("lmai").warning("LM_API_TOKEN 为空，/api/ai 接口不鉴权")

    @asynccontextmanager
    async def lifespan(_app):  # type: ignore[no-untyped-def]
        svc.start()
        try:
            yield
        finally:
            await svc.stop()
            await svc.mysql.close()
            await svc.redis.r.aclose()

    app = create_app(svc, lifespan)
    uvicorn.run(app, host=cfg.listen_host, port=cfg.listen_port, log_level="warning", access_log=False)
    return 0


if __name__ == "__main__":
    sys.exit(main())
