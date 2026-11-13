add_library(lm_common STATIC
  src/config.cpp
  src/log.cpp)
target_include_directories(lm_common PUBLIC include ${PROJECT_SOURCE_DIR}/kmod/cpustat)
target_link_libraries(lm_common PUBLIC spdlog::spdlog yaml-cpp::yaml-cpp Threads::Threads)
