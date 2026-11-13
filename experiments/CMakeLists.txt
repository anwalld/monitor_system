add_executable(lm-bench lm_bench.cpp)
target_link_libraries(lm-bench PRIVATE lm_proto)
