# Manager - 管理者服务器
# 部署在管理端，接收工作者推送的数据、存储到数据库、提供查询服务

# 查找 gRPC
find_package(gRPC CONFIG QUIET)
if(NOT gRPC_FOUND)
    find_library(GRPC_LIBRARY grpc)
    find_library(GRPCPP_LIBRARY grpc++)
    if(GRPC_LIBRARY AND GRPCPP_LIBRARY)
        add_library(grpc_lib INTERFACE)
        target_link_libraries(grpc_lib INTERFACE ${GRPC_LIBRARY} ${GRPCPP_LIBRARY})
        add_library(gRPC::grpc++ ALIAS grpc_lib)
    endif()
endif()

# MySQL 支持选项
option(ENABLE_MYSQL "Enable MySQL support for data persistence" ON)

set(MANAGER_SOURCES
    src/main.cpp
    src/host_manager.cpp
    src/query_manager.cpp
    src/rpc/grpc_server.cpp
    src/rpc/query_service.cpp
)

# 可执行文件: manager (管理者服务器)
add_executable(manager ${MANAGER_SOURCES})

target_include_directories(manager PUBLIC
    ${PROJECT_SOURCE_DIR}/manager/include
)

# 基础链接库
set(MANAGER_LIBS monitor_proto gRPC::grpc++)

# MySQL 依赖（可选）
if(ENABLE_MYSQL)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(MYSQL mysqlclient)
    endif()
    
    if(MYSQL_FOUND)
        message(STATUS "MySQL support enabled")
        target_compile_definitions(manager PRIVATE ENABLE_MYSQL=1)
        target_include_directories(manager PUBLIC ${MYSQL_INCLUDE_DIRS})
        list(APPEND MANAGER_LIBS ${MYSQL_LIBRARIES})
    else()
        message(WARNING "MySQL not found, disabling MySQL support")
    endif()
endif()

target_link_libraries(manager PUBLIC ${MANAGER_LIBS})

# 历史查询客户端，与 Manager 共用协议，不依赖 MySQL 客户端库
add_executable(query_client
    src/query_main.cpp
    src/rpc/rpc_client.cpp
)
target_include_directories(query_client PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)
target_link_libraries(query_client PRIVATE monitor_proto gRPC::grpc++)
