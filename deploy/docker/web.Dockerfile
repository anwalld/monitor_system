# 看板镜像（M7）：node 构建 Vue 应用 → nginx 托管静态文件并反代 Manager。
# 构建上下文为仓库根目录：docker build -f deploy/docker/web.Dockerfile -t lm-web .
# 运行：HTTP_PORT（默认 80）、MANAGER_HTTP（默认 127.0.0.1:8080）；compose 里用 host 网络。

FROM node:22-alpine AS build
WORKDIR /web
COPY web/package.json web/package-lock.json ./
RUN npm ci --no-audit --no-fund
COPY web/ ./
RUN npm run build

FROM nginx:1.27-alpine
ENV HTTP_PORT=80 MANAGER_HTTP=127.0.0.1:8080
COPY deploy/nginx/default.conf.template /etc/nginx/templates/default.conf.template
COPY --from=build /web/dist /usr/share/nginx/html
