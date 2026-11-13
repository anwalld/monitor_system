# 查找 Protobuf 与 gRPC，并提供 lm_add_proto_library()。
# 优先用 CMake config（Homebrew、源码安装）；Ubuntu apt 的 gRPC 没有 config 文件，回退到 pkg-config。

find_package(Protobuf CONFIG QUIET)
if(NOT Protobuf_FOUND)
  find_package(Protobuf REQUIRED)
endif()

find_package(gRPC CONFIG QUIET)
if(gRPC_FOUND)
  set(LM_GRPCPP_TARGET gRPC::grpc++)
  set(LM_GRPC_CPP_PLUGIN $<TARGET_FILE:gRPC::grpc_cpp_plugin>)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(GRPCPP REQUIRED IMPORTED_TARGET grpc++)
  set(LM_GRPCPP_TARGET PkgConfig::GRPCPP)
  find_program(LM_GRPC_CPP_PLUGIN grpc_cpp_plugin REQUIRED)
endif()

if(TARGET protobuf::protoc)
  set(LM_PROTOC $<TARGET_FILE:protobuf::protoc>)
else()
  find_program(LM_PROTOC protoc REQUIRED)
endif()
if(TARGET protobuf::libprotobuf)
  set(LM_PROTOBUF_TARGET protobuf::libprotobuf)
else()
  set(LM_PROTOBUF_TARGET ${Protobuf_LIBRARIES})
endif()

# lm_add_proto_library(<target> <proto files...>)
# 生成 .pb 与 .grpc.pb 源文件，编进静态库；头文件以 "<name>.pb.h" 形式包含。
function(lm_add_proto_library target)
  set(gen_dir ${CMAKE_CURRENT_BINARY_DIR}/gen)
  file(MAKE_DIRECTORY ${gen_dir})
  set(srcs)
  foreach(proto ${ARGN})
    get_filename_component(abs ${proto} ABSOLUTE)
    get_filename_component(name ${proto} NAME_WE)
    get_filename_component(dir ${abs} DIRECTORY)
    set(out ${gen_dir}/${name}.pb.cc ${gen_dir}/${name}.pb.h
            ${gen_dir}/${name}.grpc.pb.cc ${gen_dir}/${name}.grpc.pb.h)
    add_custom_command(
      OUTPUT ${out}
      COMMAND ${LM_PROTOC}
        --proto_path=${dir}
        --cpp_out=${gen_dir}
        --grpc_out=${gen_dir}
        --plugin=protoc-gen-grpc=${LM_GRPC_CPP_PLUGIN}
        ${abs}
      DEPENDS ${abs}
      COMMENT "protoc ${name}.proto"
      VERBATIM)
    list(APPEND srcs ${out})
  endforeach()
  add_library(${target} STATIC ${srcs})
  target_include_directories(${target} PUBLIC ${gen_dir})
  target_link_libraries(${target} PUBLIC ${LM_GRPCPP_TARGET} ${LM_PROTOBUF_TARGET})
endfunction()
