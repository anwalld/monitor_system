# cpustat 内核模块（Kbuild）
#   make                 编译 cpustat.ko（需要 linux-headers-$(uname -r)）
#   make test            编译用户态测试程序 test/cpustat_test
#   make clean
ifneq ($(KERNELRELEASE),)
# 由内核构建系统调用
obj-m := cpustat.o
ccflags-y := -I$(src)
else

KDIR ?= /lib/modules/$(shell uname -r)/build

all:
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

test: test/cpustat_test

test/cpustat_test: test/cpustat_test.c cpustat_uapi.h
	$(CC) -O2 -Wall -Wextra -I$(CURDIR) -o $@ $<

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
	rm -f test/cpustat_test

.PHONY: all test clean
endif
