add_library(lm_worker_collector STATIC
  collector/cpu_busy.cpp
  collector/proc_readers.cpp
  collector/collector.cpp
  collector/shm_cpu_source.cpp
  collector/interval_guard.cpp
  collector/runq_histogram.cpp
  collector/runq_probe.cpp)
target_include_directories(lm_worker_collector PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(lm_worker_collector PUBLIC lm_common lm_proto)
target_include_directories(lm_worker_collector PUBLIC ${PROJECT_SOURCE_DIR}/bpf)  # runqlat.h
if(LM_HAVE_BPF)
  target_compile_definitions(lm_worker_collector PUBLIC LM_HAVE_BPF)
  target_link_libraries(lm_worker_collector PUBLIC lm_bpf_runqlat)
endif()

# 网卡链路质量（M4）：Linux 专用部分在源文件内按 __linux__ 编译，其他平台为桩
add_library(lm_worker_netprobe STATIC
  netprobe/nic_config.cpp
  netprobe/route.cpp
  netprobe/icmp.cpp
  netprobe/probe_state.cpp
  netprobe/prober.cpp
  netprobe/retrans.cpp
  netprobe/sock_diag.cpp
  netprobe/traffic.cpp
  netprobe/nic_monitor.cpp)
target_include_directories(lm_worker_netprobe PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/netprobe)
target_link_libraries(lm_worker_netprobe PUBLIC lm_common lm_proto)

add_library(lm_worker_reporter STATIC
  reporter/reporter.cpp)
target_include_directories(lm_worker_reporter PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(lm_worker_reporter PUBLIC lm_common lm_proto)

# 任务执行（M5）：task_runner 是纯 POSIX，task_service/event_sender 依赖 gRPC
add_library(lm_worker_task_runner STATIC
  task/task_runner.cpp)
target_include_directories(lm_worker_task_runner PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/task)

add_library(lm_worker_task STATIC
  task/event_sender.cpp
  task/rescue_sampler.cpp
  task/task_service.cpp)
target_include_directories(lm_worker_task PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/task)
target_link_libraries(lm_worker_task PUBLIC lm_worker_task_runner lm_worker_collector lm_common lm_proto)

add_executable(worker main.cpp)
target_link_libraries(worker PRIVATE lm_worker_collector lm_worker_reporter lm_worker_netprobe lm_worker_task)

# CPU 读取耗时基准（M2 验收）
add_executable(cpu_read_bench bench/cpu_read_bench.cpp)
target_link_libraries(cpu_read_bench PRIVATE lm_worker_collector)

# 调度延迟对照工具（M3 验收）
add_executable(runq_compare bench/runq_compare.cpp)
target_link_libraries(runq_compare PRIVATE lm_worker_collector)
