"""LLM 诊断输出的 schema（pydantic 校验）。"""

from __future__ import annotations

import json
import re
from typing import Literal

from pydantic import BaseModel, ConfigDict, Field


class Cause(BaseModel):
    model_config = ConfigDict(extra="forbid")
    cause: str = Field(min_length=1, max_length=300)
    evidence: list[str] = Field(min_length=1, max_length=8)
    confidence: float = Field(ge=0.0, le=1.0)


class Report(BaseModel):
    model_config = ConfigDict(extra="forbid")
    severity: Literal["low", "medium", "high"]
    summary: str = Field(min_length=1, max_length=200)
    causes: list[Cause] = Field(min_length=1, max_length=5)
    actions: list[str] = Field(default_factory=list, max_length=5)


SCHEMA_TEXT = """{
  "severity": "low | medium | high",
  "summary": "一句话结论（不超过 200 字）",
  "causes": [
    {"cause": "可能的原因", "evidence": ["引用上下文里的具体数值"], "confidence": 0.0}
  ],
  "actions": ["建议的人工操作"]
}"""

_FENCE = re.compile(r"^\s*```(?:json)?\s*(.*?)\s*```\s*$", re.S)


def parse_report(text: str) -> Report:
    """解析模型输出。容忍 ```json 代码块包裹；其余格式问题抛 ValueError / ValidationError。"""
    if text is None:
        raise ValueError("模型没有返回内容")
    m = _FENCE.match(text)
    if m:
        text = m.group(1)
    data = json.loads(text)
    return Report.model_validate(data)
