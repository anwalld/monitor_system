# AI 服务镜像（M8）：Python 3.11 + FastAPI，端口 8000。
# 构建上下文为仓库根目录：docker build -f deploy/docker/ai.Dockerfile -t lm-ai .
# 运行：LM_REDIS_URI、LM_REDIS_PASSWORD、LM_MYSQL_*、LM_API_TOKEN、各厂商 API Key（见 config/ai.yaml 的 key_env）。

FROM python:3.11-slim
ENV PYTHONDONTWRITEBYTECODE=1 PYTHONUNBUFFERED=1
WORKDIR /app
COPY ai/requirements.txt ./
RUN pip install --no-cache-dir -r requirements.txt
COPY ai/ ./
COPY config/ai.yaml /etc/lm/ai.yaml
RUN useradd -r -u 10001 lm
USER lm
EXPOSE 8000
CMD ["python", "-m", "lmai.main", "-c", "/etc/lm/ai.yaml"]
