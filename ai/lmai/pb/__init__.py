"""grpc_tools.protoc 生成的 proto 代码（monitor_pb2、advisor_pb2、advisor_pb2_grpc），不进 Git。

生成：python -m grpc_tools.protoc -I proto --python_out=ai/lmai/pb --grpc_python_out=ai/lmai/pb \
      proto/monitor.proto proto/advisor.proto
生成的 advisor_pb2.py 用 `import monitor_pb2` 这种顶层导入，所以把本目录加进 sys.path。
"""

import os
import sys

_here = os.path.dirname(os.path.abspath(__file__))
if _here not in sys.path:
    sys.path.insert(0, _here)
