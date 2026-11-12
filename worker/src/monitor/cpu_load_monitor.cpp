#include "monitor/cpu_load_monitor.h"
#include "monitor/monitor_structs.h"
#include "monitor_info.grpc.pb.h"
#include "monitor_info.pb.h"
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>

namespace monitor {

// 从 /proc/loadavg 读取负载信息作为后备方案
static bool ReadLoadFromProc(float* load1, float* load3, float* load15) {
    FILE* fp = fopen("/proc/loadavg", "r");
    if (!fp) return false;
    
    int ret = fscanf(fp, "%f %f %f", load1, load3, load15);
    fclose(fp);
    return ret == 3;
}

void CpuLoadMonitor::UpdateOnce(monitor::proto::MonitorInfo* monitor_info) {
    // 首先尝试从内核模块读取
    int fd = open("/dev/cpu_load_monitor", O_RDONLY);
    if (fd >= 0) {
        size_t load_size = sizeof(struct cpu_load);
        void* addr = mmap(nullptr, load_size, PROT_READ, MAP_SHARED, fd, 0);
        if (addr != MAP_FAILED) {
            struct cpu_load info;
            memcpy(&info, addr, load_size);

            auto cpu_load_msg = monitor_info->mutable_cpu_load();
            cpu_load_msg->set_load_avg_1(info.load_avg_1);
            cpu_load_msg->set_load_avg_3(info.load_avg_3);
            cpu_load_msg->set_load_avg_15(info.load_avg_15);

            munmap(addr, load_size);
            close(fd);
            return;
        }
        close(fd);
    }
    
    // 后备方案：从 /proc/loadavg 读取
    float load1, load3, load15;
    if (ReadLoadFromProc(&load1, &load3, &load15)) {
        auto cpu_load_msg = monitor_info->mutable_cpu_load();
        cpu_load_msg->set_load_avg_1(load1);
        cpu_load_msg->set_load_avg_3(load3);
        cpu_load_msg->set_load_avg_15(load15);
    }
}
}  // namespace monitor