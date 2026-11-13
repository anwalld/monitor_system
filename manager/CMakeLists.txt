find_package(redis++ CONFIG QUIET)
if(redis++_FOUND)
  if(TARGET redis++::redis++_static)
    set(LM_REDISPP_TARGET redis++::redis++_static)
  else()
    set(LM_REDISPP_TARGET redis++::redis++)
  endif()
else()
  # Ubuntu apt 的 libredis++-dev 没有 CMake config；Homebrew 没有 redis++，从源码构建
  find_path(REDISPP_INCLUDE_DIR sw/redis++/redis++.h)
  find_library(REDISPP_LIBRARY redis++)
  find_library(HIREDIS_LIBRARY hiredis REQUIRED)
  if(REDISPP_INCLUDE_DIR AND REDISPP_LIBRARY)
    add_library(lm_redispp INTERFACE)
    target_include_directories(lm_redispp INTERFACE ${REDISPP_INCLUDE_DIR})
    target_link_libraries(lm_redispp INTERFACE ${REDISPP_LIBRARY} ${HIREDIS_LIBRARY})
    set(LM_REDISPP_TARGET lm_redispp)
  else()
    message(STATUS "redis-plus-plus not found, building from source")
    include(FetchContent)
    set(REDIS_PLUS_PLUS_BUILD_TEST OFF CACHE BOOL "" FORCE)
    set(REDIS_PLUS_PLUS_BUILD_SHARED OFF CACHE BOOL "" FORCE)
    set(REDIS_PLUS_PLUS_CXX_STANDARD 17 CACHE STRING "" FORCE)
    set(CMAKE_POLICY_VERSION_MINIMUM 3.5)  # 上游 cmake_minimum_required 版本过低，CMake 4 需要
    FetchContent_Declare(redis_plus_plus
      URL https://github.com/sewenew/redis-plus-plus/archive/refs/tags/1.3.13.tar.gz
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
    FetchContent_MakeAvailable(redis_plus_plus)
    # 上游在某些配置下没有把生成的 hiredis_features.h 所在目录加进接口头文件路径
    target_include_directories(redis++_static PUBLIC $<BUILD_INTERFACE:${redis_plus_plus_BINARY_DIR}/src>)
    target_link_libraries(redis++_static PUBLIC ${HIREDIS_LIBRARY})
    set(LM_REDISPP_TARGET redis++_static)
  endif()
endif()

add_library(lm_manager_scoring STATIC
  scoring/scoring_params.cpp
  scoring/score.cpp
  scoring/eviction.cpp
  scoring/selector.cpp)
target_include_directories(lm_manager_scoring PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(lm_manager_scoring PUBLIC lm_common lm_proto)

add_library(lm_manager_ingest STATIC
  breaker/breaker_params.cpp
  breaker/rescue_logic.cpp
  breaker/node_state.cpp
  inflight/inflight_ledger.cpp
  ingest/node_table.cpp
  ingest/collector_service.cpp)
target_include_directories(lm_manager_ingest PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(lm_manager_ingest PUBLIC lm_common lm_proto lm_manager_scoring)

add_library(lm_manager_storage STATIC
  storage/redis_writer.cpp)
target_include_directories(lm_manager_storage PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(lm_manager_storage PUBLIC lm_manager_ingest ${LM_REDISPP_TARGET})

add_library(lm_manager_api STATIC
  api/scheduler_service.cpp
  breaker/prober.cpp
  breaker/rescue_session.cpp
  scheduler_loop.cpp)
target_include_directories(lm_manager_api PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(lm_manager_api PUBLIC lm_manager_ingest)

add_executable(manager main.cpp)
target_link_libraries(manager PRIVATE lm_manager_ingest lm_manager_storage lm_manager_api)
