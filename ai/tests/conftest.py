import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

import pytest  # noqa: E402

from lmai import config  # noqa: E402

ROOT = os.path.join(os.path.dirname(__file__), "..", "..")


def _gen_pb():
    """lmai/pb 的生成代码缺失时生成一次（镜像构建时已生成；本机测试需要 grpcio-tools）。"""
    pb = os.path.join(os.path.dirname(__file__), "..", "lmai", "pb")
    if os.path.exists(os.path.join(pb, "advisor_pb2_grpc.py")):
        return
    proto = os.path.join(ROOT, "proto")
    if not os.path.isdir(proto):
        return
    try:
        from grpc_tools import protoc
    except ImportError:
        return
    protoc.main(["protoc", f"-I{proto}", f"--python_out={pb}", f"--grpc_python_out={pb}",
                 os.path.join(proto, "monitor.proto"), os.path.join(proto, "advisor.proto")])


_gen_pb()


@pytest.fixture
def cfg():
    path = os.path.join(ROOT, "config", "ai.yaml")
    if not os.path.exists(path):  # 镜像里配置在 /etc/lm/ai.yaml
        path = "/etc/lm/ai.yaml"
    return config.load(path)


def sample(node="w1", ts=0, cpu=0.03, mem=0.8, disk=0.01, runq=80.0, drop=0.0, nics=None):
    s = {"node_id": node, "ts_ms": str(ts), "cpu_busy": cpu, "mem_avail_ratio": mem, "disk_util": disk,
         "runq_p50_us": 20.0, "runq_p99_us": runq, "runq_drop_rate": drop}
    if nics is None:
        nics = [{"name": "eth0", "rtt_ms": 1.0, "timeout_rate": 0.0, "retrans_rate": 0.0}]
    s["nics"] = nics
    return s
