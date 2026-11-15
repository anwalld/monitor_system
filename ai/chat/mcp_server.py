"""运维问答 MCP server（M9，stdio）：把 lmai.tools 的六个只读工具注册给 MCP 客户端（如 Claude Code）。

运行（在镜像里，工作目录 /app）：
  python -m chat.mcp_server [-c /etc/lm/ai.yaml]
配置与 AI 服务相同：配置文件 + 环境变量 LM_REDIS_URI、LM_REDIS_PASSWORD、LM_MYSQL_*。
只读：不提供剔除、重启等写操作。
"""

from __future__ import annotations

import argparse
import logging
import os
import sys
from typing import Any

from mcp.server.fastmcp import FastMCP

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))

from lmai import config as config_mod  # noqa: E402
from lmai.store import MySQL, Redis  # noqa: E402
from lmai.tools import SPEC_BY_NAME, Tools  # noqa: E402


def build(cfg: config_mod.Config) -> FastMCP:
    tools = Tools(cfg, Redis(cfg), MySQL(cfg))
    mcp = FastMCP("lm-ops")

    def doc(name: str) -> str:
        return SPEC_BY_NAME[name]["description"]

    @mcp.tool(name="list_nodes", description=doc("list_nodes"))
    async def list_nodes() -> dict[str, Any]:
        return await tools.call("list_nodes", {})

    @mcp.tool(name="get_node_metrics", description=doc("get_node_metrics"))
    async def get_node_metrics(node: str, metric: str, minutes: int = 10) -> dict[str, Any]:
        return await tools.call("get_node_metrics", {"node": node, "metric": metric, "minutes": minutes})

    @mcp.tool(name="get_node_events", description=doc("get_node_events"))
    async def get_node_events(node: str, hours: float = 24) -> dict[str, Any]:
        return await tools.call("get_node_events", {"node": node, "hours": hours})

    @mcp.tool(name="explain_score", description=doc("explain_score"))
    async def explain_score(node: str) -> dict[str, Any]:
        return await tools.call("explain_score", {"node": node})

    @mcp.tool(name="get_task_stats", description=doc("get_task_stats"))
    async def get_task_stats(node: str = "", hours: float = 1) -> dict[str, Any]:
        return await tools.call("get_task_stats", {"node": node, "hours": hours})

    @mcp.tool(name="get_diagnoses", description=doc("get_diagnoses"))
    async def get_diagnoses(node: str = "", hours: float = 24) -> dict[str, Any]:
        return await tools.call("get_diagnoses", {"node": node, "hours": hours})

    return mcp


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="lm-mcp")
    ap.add_argument("-c", "--config", default=os.environ.get("LM_AI_CONFIG", "/etc/lm/ai.yaml"))
    args = ap.parse_args(argv)
    logging.basicConfig(level="WARNING", stream=sys.stderr)  # stdout 留给 MCP 协议
    build(config_mod.load(args.config)).run()  # 默认 stdio
    return 0


if __name__ == "__main__":
    sys.exit(main())
