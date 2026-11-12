cmake_minimum_required(VERSION 3.10.2)
project(monitor_system LANGUAGES CXX)

set(CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
set(CMAKE_CXX_STANDARD 17)

option(BUILD_MANAGER "Build manager server (requires MySQL)" ON)

# Proto 定义（必须先编译）
add_subdirectory(proto)

# Worker - 工作者服务器，部署在被监控机器上，采集性能数据并推送给管理者
add_subdirectory(worker)

# Manager - 管理者服务器，部署在管理端，接收数据、存储、提供查询服务
if(BUILD_MANAGER)
    add_subdirectory(manager)
endif()
