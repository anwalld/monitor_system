# Manager 镜像：多阶段构建，运行阶段只装运行时库。
# 构建上下文为仓库根目录：docker build -f deploy/docker/manager.Dockerfile -t lm-manager .

FROM ubuntu:24.04 AS build
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build git ca-certificates pkg-config \
        libgrpc++-dev protobuf-compiler-grpc libprotobuf-dev protobuf-compiler \
        libspdlog-dev libyaml-cpp-dev libhiredis-dev \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /src
COPY CMakeLists.txt ./
COPY cmake cmake
COPY proto proto
COPY common common
COPY worker worker
COPY manager manager
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DLM_BUILD_TESTS=OFF \
    && cmake --build build --target manager

FROM ubuntu:24.04
ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        libgrpc++1.51t64 libgrpc29t64 libprotobuf32t64 libabsl20220623t64 libre2-10 libcares2 \
        libspdlog1.12 libfmt9 libyaml-cpp0.8 libhiredis1.1.0 libssl3t64 zlib1g ca-certificates \
    && rm -rf /var/lib/apt/lists/* \
    && useradd --system --no-create-home lm
COPY --from=build /src/build/manager/manager /usr/local/bin/manager
COPY config/manager.yaml /etc/lm/manager.yaml
USER lm
EXPOSE 50051
ENTRYPOINT ["/usr/local/bin/manager", "-c", "/etc/lm/manager.yaml"]
