cmake_minimum_required(VERSION 3.20)
project(linux_monitor LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

option(LM_BUILD_MANAGER "Build manager (needs redis-plus-plus)" ON)
option(LM_BUILD_TESTS "Build unit tests" ON)
option(LM_ENABLE_TSAN "Build with ThreadSanitizer" OFF)

if(LM_ENABLE_TSAN)
  add_compile_options(-fsanitize=thread -g -O1)
  add_link_options(-fsanitize=thread)
endif()

find_package(Threads REQUIRED)
find_package(spdlog REQUIRED)
find_package(yaml-cpp REQUIRED)
include(cmake/grpc.cmake)

if(LM_BUILD_TESTS)
  include(FetchContent)
  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
    URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
  include(GoogleTest)
  enable_testing()
endif()

add_subdirectory(proto)
add_subdirectory(common)
add_subdirectory(worker)
if(LM_BUILD_MANAGER)
  add_subdirectory(manager)
endif()
if(LM_BUILD_TESTS)
  add_subdirectory(tests)
endif()
