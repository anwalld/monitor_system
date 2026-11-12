/**
 * test_net_ebpf.cpp - eBPF TC Hook 网络监控测试程序
 * 
 * 编译:
 * g++ -o test_net_ebpf test_net_ebpf.cpp -I../../include -lbpf -lelf -lz -std=c++17
 * 
 * 运行 (需要 root 权限):
 * sudo ./test_net_ebpf
 */

#include <bpf/libbpf.h>
#include <bpf/bpf.h>
#include <net/if.h>
#include <linux/if_link.h>
#include <unistd.h>
#include <dirent.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <csignal>
#include <vector>

// 包含生成的 skeleton
#include "monitor/net_stats.skel.h"

// 共享数据结构
struct net_stats {
    uint64_t rcv_bytes;
    uint64_t rcv_packets;
    uint64_t snd_bytes;
    uint64_t snd_packets;
};

static volatile bool running = true;
static std::vector<uint32_t> attached_ifindexes;

void sig_handler(int sig) {
    running = false;
}

// 获取所有网卡 ifindex
std::vector<uint32_t> get_all_ifindexes() {
    std::vector<uint32_t> indexes;
    DIR* dir = opendir("/sys/class/net");
    if (!dir) return indexes;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') continue;
        if (strcmp(entry->d_name, "lo") == 0) continue;  // 跳过 loopback
        
        unsigned int ifindex = if_nametoindex(entry->d_name);
        if (ifindex > 0) {
            indexes.push_back(ifindex);
        }
    }
    closedir(dir);
    return indexes;
}

int main() {
    struct net_stats_bpf* skel = nullptr;
    int err;

    // 设置信号处理
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);

    printf("Loading eBPF TC hook program...\n");

    // 打开 BPF skeleton
    skel = net_stats_bpf__open();
    if (!skel) {
        fprintf(stderr, "Failed to open BPF skeleton\n");
        return 1;
    }

    // 加载 BPF 程序
    err = net_stats_bpf__load(skel);
    if (err) {
        fprintf(stderr, "Failed to load BPF program: %s\n", strerror(-err));
        net_stats_bpf__destroy(skel);
        return 1;
    }

    printf("BPF program loaded, attaching TC hooks...\n");

    // 获取程序 fd
    int ingress_fd = bpf_program__fd(skel->progs.tc_ingress);
    int egress_fd = bpf_program__fd(skel->progs.tc_egress);

    // 获取所有网卡并附加 TC hook
    auto ifindexes = get_all_ifindexes();
    
    for (uint32_t ifindex : ifindexes) {
        char ifname[IF_NAMESIZE];
        if (if_indextoname(ifindex, ifname) == nullptr) continue;

        // 先创建 clsact qdisc
        char cmd[256];
        snprintf(cmd, sizeof(cmd), "tc qdisc add dev %s clsact 2>/dev/null", ifname);
        system(cmd);

        // 创建 TC hook
        LIBBPF_OPTS(bpf_tc_hook, hook,
            .ifindex = static_cast<int>(ifindex),
            .attach_point = BPF_TC_INGRESS,
        );
        
        err = bpf_tc_hook_create(&hook);
        if (err && err != -EEXIST) {
            fprintf(stderr, "Failed to create TC hook for %s: %s\n", 
                    ifname, strerror(-err));
            continue;
        }

        // 附加 ingress
        LIBBPF_OPTS(bpf_tc_opts, opts_in,
            .prog_fd = ingress_fd,
        );
        err = bpf_tc_attach(&hook, &opts_in);
        if (err) {
            fprintf(stderr, "Failed to attach ingress for %s: %s\n", 
                    ifname, strerror(-err));
        } else {
            printf("Attached TC ingress to %s (ifindex=%u)\n", ifname, ifindex);
            attached_ifindexes.push_back(ifindex);
        }

        // 附加 egress
        hook.attach_point = BPF_TC_EGRESS;
        LIBBPF_OPTS(bpf_tc_opts, opts_eg,
            .prog_fd = egress_fd,
        );
        err = bpf_tc_attach(&hook, &opts_eg);
        if (err) {
            fprintf(stderr, "Failed to attach egress for %s: %s\n", 
                    ifname, strerror(-err));
        } else {
            printf("Attached TC egress to %s\n", ifname);
        }
    }

    if (attached_ifindexes.empty()) {
        fprintf(stderr, "No interfaces attached!\n");
        net_stats_bpf__destroy(skel);
        return 1;
    }

    printf("\neBPF TC hooks attached successfully!\n");
    printf("Monitoring network traffic... Press Ctrl+C to stop.\n\n");

    int map_fd = bpf_map__fd(skel->maps.net_stats_map);

    // 上一次的统计数据
    struct {
        uint64_t rcv_bytes;
        uint64_t snd_bytes;
    } prev_stats[64] = {};

    while (running) {
        sleep(2);

        printf("\n--- Network Statistics (TC Hook) ---\n");
        printf("%-12s %15s %15s %15s %15s\n", 
               "Interface", "RX bytes/s", "TX bytes/s", "RX pkts", "TX pkts");

        uint32_t key = 0, next_key;
        struct net_stats stats;

        while (bpf_map_get_next_key(map_fd, &key, &next_key) == 0) {
            if (bpf_map_lookup_elem(map_fd, &next_key, &stats) == 0) {
                char ifname[IF_NAMESIZE];
                if (if_indextoname(next_key, ifname) != nullptr) {
                    // 计算速率 (2秒间隔)
                    uint64_t rx_rate = (stats.rcv_bytes - prev_stats[next_key].rcv_bytes) / 2;
                    uint64_t tx_rate = (stats.snd_bytes - prev_stats[next_key].snd_bytes) / 2;

                    printf("%-12s %15lu %15lu %15lu %15lu\n",
                           ifname, rx_rate, tx_rate, 
                           stats.rcv_packets, stats.snd_packets);

                    prev_stats[next_key].rcv_bytes = stats.rcv_bytes;
                    prev_stats[next_key].snd_bytes = stats.snd_bytes;
                }
            }
            key = next_key;
        }
    }

    printf("\nCleaning up TC hooks...\n");
    
    // 分离 TC hook
    for (uint32_t ifindex : attached_ifindexes) {
        LIBBPF_OPTS(bpf_tc_hook, hook,
            .ifindex = static_cast<int>(ifindex),
            .attach_point = BPF_TC_INGRESS,
        );
        LIBBPF_OPTS(bpf_tc_opts, opts);
        bpf_tc_detach(&hook, &opts);
        
        hook.attach_point = BPF_TC_EGRESS;
        bpf_tc_detach(&hook, &opts);
        
        char ifname[IF_NAMESIZE];
        if (if_indextoname(ifindex, ifname) != nullptr) {
            printf("Detached TC hooks from %s\n", ifname);
        }
    }

    net_stats_bpf__destroy(skel);
    printf("Done.\n");

    return 0;
}
