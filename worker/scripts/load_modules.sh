#!/bin/bash
#
# 加载监控内核模块脚本
#

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
KMOD_DIR="${SCRIPT_DIR}/../src/kmod"

echo "=== Monitor Kernel Modules Loader ==="

# 检查是否以 root 权限运行
if [ "$EUID" -ne 0 ]; then
    echo "Error: Please run as root (sudo)"
    exit 1
fi

# 编译内核模块（如果需要）
compile_modules() {
    echo "Compiling kernel modules..."
    cd "$KMOD_DIR"
    make clean
    make
    if [ $? -ne 0 ]; then
        echo "Error: Failed to compile kernel modules"
        exit 1
    fi
    echo "Compilation successful"
}

# 加载软中断采集模块
load_softirq_module() {
    local module_path="${KMOD_DIR}/softirq_collector.ko"
    
    if [ ! -f "$module_path" ]; then
        echo "Module not found: $module_path"
        echo "Compiling..."
        compile_modules
    fi
    
    # 检查模块是否已加载
    if lsmod | grep -q "softirq_collector"; then
        echo "softirq_collector already loaded"
    else
        echo "Loading softirq_collector..."
        insmod "$module_path"
        if [ $? -eq 0 ]; then
            echo "softirq_collector loaded successfully"
        else
            echo "Error: Failed to load softirq_collector"
            return 1
        fi
    fi
    
    # 设置设备权限
    if [ -e /dev/cpu_softirq_monitor ]; then
        chmod 644 /dev/cpu_softirq_monitor
        echo "Device /dev/cpu_softirq_monitor ready"
    fi
}

# 加载 CPU 状态采集模块
load_cpu_stat_module() {
    local module_path="${KMOD_DIR}/cpu_stat_collector.ko"
    
    if [ ! -f "$module_path" ]; then
        echo "Module not found: $module_path"
        echo "Compiling..."
        compile_modules
    fi
    
    # 检查模块是否已加载
    if lsmod | grep -q "cpu_stat_collector"; then
        echo "cpu_stat_collector already loaded"
    else
        echo "Loading cpu_stat_collector..."
        insmod "$module_path"
        if [ $? -eq 0 ]; then
            echo "cpu_stat_collector loaded successfully"
        else
            echo "Error: Failed to load cpu_stat_collector"
            return 1
        fi
    fi
    
    # 设置设备权限
    if [ -e /dev/cpu_stat_monitor ]; then
        chmod 644 /dev/cpu_stat_monitor
        echo "Device /dev/cpu_stat_monitor ready"
    fi
}

# 加载所有模块
load_all_modules() {
    load_softirq_module
    load_cpu_stat_module
}

# 卸载模块
unload_modules() {
    echo "Unloading kernel modules..."
    
    if lsmod | grep -q "softirq_collector"; then
        rmmod softirq_collector
        echo "softirq_collector unloaded"
    fi
    
    if lsmod | grep -q "cpu_stat_collector"; then
        rmmod cpu_stat_collector
        echo "cpu_stat_collector unloaded"
    fi
}

# 显示状态
show_status() {
    echo ""
    echo "=== Module Status ==="
    lsmod | grep -E "softirq_collector|cpu_stat_collector|cpu_load" || echo "No monitor modules loaded"
    
    echo ""
    echo "=== Device Files ==="
    ls -la /dev/cpu_* 2>/dev/null || echo "No monitor devices found"
    
    echo ""
    echo "=== Recent Kernel Log ==="
    dmesg | grep -E "softirq_collector|cpu_softirq_monitor|cpu_stat_collector|cpu_stat_monitor" | tail -15
}

# 主逻辑
case "${1:-load}" in
    load)
        load_all_modules
        show_status
        ;;
    unload)
        unload_modules
        ;;
    reload)
        unload_modules
        sleep 1
        load_all_modules
        show_status
        ;;
    compile)
        compile_modules
        ;;
    status)
        show_status
        ;;
    *)
        echo "Usage: $0 {load|unload|reload|compile|status}"
        exit 1
        ;;
esac

echo ""
echo "Done."
