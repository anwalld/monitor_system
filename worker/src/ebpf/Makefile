# eBPF 程序编译 Makefile

CLANG ?= clang
BPFTOOL ?= bpftool
ARCH := $(shell uname -m | sed 's/x86_64/x86/' | sed 's/aarch64/arm64/')

# vmlinux.h 位置
VMLINUX_H ?= /usr/include/vmlinux.h

# 输出目录
OUTPUT_DIR := .output
SKEL_DIR := $(OUTPUT_DIR)

# BPF 程序源文件
BPF_SRCS := net_stats.bpf.c
BPF_OBJS := $(patsubst %.bpf.c,$(OUTPUT_DIR)/%.bpf.o,$(BPF_SRCS))
BPF_SKELS := $(patsubst %.bpf.c,$(SKEL_DIR)/%.skel.h,$(BPF_SRCS))

# 编译标志
BPF_CFLAGS := -g -O2 -target bpf -D__TARGET_ARCH_$(ARCH)
BPF_CFLAGS += -I$(dir $(VMLINUX_H))
BPF_CFLAGS += -I/usr/include

.PHONY: all clean

all: $(OUTPUT_DIR) $(BPF_SKELS)

$(OUTPUT_DIR):
	mkdir -p $(OUTPUT_DIR)

# 复制 vmlinux.h 到当前目录（如果不存在）
vmlinux.h:
	@if [ ! -f vmlinux.h ]; then \
		cp $(VMLINUX_H) vmlinux.h; \
	fi

# 编译 BPF 程序
$(OUTPUT_DIR)/%.bpf.o: %.bpf.c vmlinux.h | $(OUTPUT_DIR)
	$(CLANG) $(BPF_CFLAGS) -c $< -o $@

# 生成 skeleton 头文件
$(SKEL_DIR)/%.skel.h: $(OUTPUT_DIR)/%.bpf.o | $(OUTPUT_DIR)
	$(BPFTOOL) gen skeleton $< > $@

clean:
	rm -rf $(OUTPUT_DIR)
	rm -f vmlinux.h

# 显示编译信息
info:
	@echo "CLANG: $(CLANG)"
	@echo "BPFTOOL: $(BPFTOOL)"
	@echo "ARCH: $(ARCH)"
	@echo "VMLINUX_H: $(VMLINUX_H)"
	@echo "BPF_OBJS: $(BPF_OBJS)"
	@echo "BPF_SKELS: $(BPF_SKELS)"
