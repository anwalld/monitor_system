# AI 服务镜像（M8、M9）：Python 3.11 + FastAPI，端口 8000；内含 MCP server（python -m chat.mcp_server）。
# 构建上下文为仓库根目录：docker build -f deploy/docker/ai.Dockerfile -t lm-ai .
# 运行：LM_REDIS_URI、LM_REDIS_PASSWORD、LM_MYSQL_*、LM_API_TOKEN、各厂商 API Key（见 config/ai.yaml 的 key_env）。

FROM python:3.11-slim
ENV PYTHONDONTWRITEBYTECODE=1 PYTHONUNBUFFERED=1
WORKDIR /app
COPY ai/requirements.txt ./
RUN pip install --no-cache-dir -r requirements.txt
COPY ai/ ./
COPY proto/monitor.proto proto/advisor.proto /tmp/proto/
# Advisor 客户端代码（M9）：负载预测经 gRPC 推给 Manager
RUN python -m grpc_tools.protoc -I /tmp/proto --python_out=lmai/pb --grpc_python_out=lmai/pb \
      /tmp/proto/monitor.proto /tmp/proto/advisor.proto && rm -rf /tmp/proto
COPY config/ai.yaml /etc/lm/ai.yaml
RUN useradd -r -u 10001 lm
USER lm
EXPOSE 8000
CMD ["python", "-m", "lmai.main", "-c", "/etc/lm/ai.yaml"]
