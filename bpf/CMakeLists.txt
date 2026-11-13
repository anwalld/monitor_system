# eBPF 程序构建：vmlinux.h → runqlat.bpf.o → runqlat.skel.h，导出接口库 lm_bpf_runqlat。
# 由顶层在 LM_ENABLE_BPF 判定启用后 add_subdirectory。需要变量 LM_BPF_CLANG、LM_BPFTOOL、LM_VMLINUX_BTF
# 以及 pkg-config 目标 PkgConfig::LIBBPF。

if(CMAKE_SYSTEM_PROCESSOR MATCHES "aarch64|arm64")
  set(_bpf_arch arm64)
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
  set(_bpf_arch x86)
else()
  message(FATAL_ERROR "eBPF：不支持的架构 ${CMAKE_SYSTEM_PROCESSOR}")
endif()

set(_gen ${CMAKE_CURRENT_BINARY_DIR}/gen)
file(MAKE_DIRECTORY ${_gen})

# 内核类型头文件：从 BTF 生成，不进仓库（CO-RE 在加载时按目标内核重定位字段偏移）
add_custom_command(
  OUTPUT ${_gen}/vmlinux.h
  COMMAND ${LM_BPFTOOL} btf dump file ${LM_VMLINUX_BTF} format c > ${_gen}/vmlinux.h
  COMMENT "生成 vmlinux.h（${LM_VMLINUX_BTF}）"
  VERBATIM)

# libbpf 头文件目录（bpf/bpf_helpers.h 等）
set(_libbpf_inc "")
foreach(_d ${LIBBPF_INCLUDE_DIRS})
  list(APPEND _libbpf_inc -I${_d})
endforeach()

add_custom_command(
  OUTPUT ${_gen}/runqlat.bpf.o
  COMMAND ${LM_BPF_CLANG} -g -O2 -target bpf -D__TARGET_ARCH_${_bpf_arch}
          -I${_gen} -I${CMAKE_CURRENT_SOURCE_DIR} ${_libbpf_inc}
          -Wall -Werror -Wno-missing-declarations  # vmlinux.h 里的匿名声明会触发该警告
          -c ${CMAKE_CURRENT_SOURCE_DIR}/runqlat.bpf.c -o ${_gen}/runqlat.bpf.o
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/runqlat.bpf.c ${CMAKE_CURRENT_SOURCE_DIR}/runqlat.h
          ${_gen}/vmlinux.h
  COMMENT "编译 runqlat.bpf.o"
  VERBATIM)

add_custom_command(
  OUTPUT ${_gen}/runqlat.skel.h
  COMMAND ${LM_BPFTOOL} gen skeleton ${_gen}/runqlat.bpf.o name runqlat_bpf > ${_gen}/runqlat.skel.h
  DEPENDS ${_gen}/runqlat.bpf.o
  COMMENT "生成 runqlat.skel.h"
  VERBATIM)

add_custom_target(lm_bpf_runqlat_gen DEPENDS ${_gen}/runqlat.skel.h)

add_library(lm_bpf_runqlat INTERFACE)
add_dependencies(lm_bpf_runqlat lm_bpf_runqlat_gen)
target_include_directories(lm_bpf_runqlat INTERFACE ${_gen} ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(lm_bpf_runqlat INTERFACE PkgConfig::LIBBPF)
