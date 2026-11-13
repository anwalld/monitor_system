# C++ 构建环境：与 Worker 机相同的 Ubuntu 24.04，用于 Linux 下构建、跑单元测试，
# 也作为 manager 镜像的构建阶段。clang、libbpf-dev、linux-tools（bpftool）用于编译 eBPF 程序（M3）。Ubuntu 24.04 没有 redis-plus-plus 包，由 CMake 从源码构建。
#   docker build -f deploy/docker/builder.Dockerfile -t lm-builder .
#   docker run --rm -v "$PWD":/src -w /src lm-builder \
#     bash -c 'cmake -S . -B build-linux -G Ninja && cmake --build build-linux && ctest --test-dir build-linux --output-on-failure'
FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive
RUN apt-get update && apt-get install -y --no-install-recommends \
        build-essential cmake ninja-build git ca-certificates pkg-config \
        libgrpc++-dev protobuf-compiler-grpc libprotobuf-dev protobuf-compiler \
        libspdlog-dev libyaml-cpp-dev libhiredis-dev \
        clang llvm libbpf-dev linux-tools-generic \
        stress procps \
    && rm -rf /var/lib/apt/lists/*
