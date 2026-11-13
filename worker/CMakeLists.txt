add_library(lm_worker_collector STATIC
  collector/cpu_busy.cpp
  collector/proc_readers.cpp
  collector/collector.cpp
  collector/shm_cpu_source.cpp
  collector/interval_guard.cpp)
target_include_directories(lm_worker_collector PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(lm_worker_collector PUBLIC lm_common lm_proto)

add_library(lm_worker_reporter STATIC
  reporter/reporter.cpp)
target_include_directories(lm_worker_reporter PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(lm_worker_reporter PUBLIC lm_common lm_proto)

add_executable(worker main.cpp)
target_link_libraries(worker PRIVATE lm_worker_collector lm_worker_reporter)

# CPU 读取耗时基准（M2 验收）
add_executable(cpu_read_bench bench/cpu_read_bench.cpp)
target_link_libraries(cpu_read_bench PRIVATE lm_worker_collector)
