cmake_minimum_required(VERSION 3.16)
project(tutuicmptunnel VERSION 1.6 LANGUAGES C)

if(NOT CMAKE_BUILD_TYPE)
  message(FATAL_ERROR "CMAKE_BUILD_TYPE is not set. Please set it to continue.\n"
                      "Example: cmake -DCMAKE_BUILD_TYPE=Release ..\n"
                      "Valid types: Release, Debug, RelWithDebInfo, MinSizeRel")
endif()

set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
add_compile_options(-Wall -Wextra -Wno-error)
add_definitions(-D_FILE_OFFSET_BITS=64 -D_GNU_SOURCE=1 -DPROJECT_NAME=${CMAKE_PROJECT_NAME} -DINSTALL_PREFIX_DIR=${CMAKE_INSTALL_PREFIX})
include_directories(common)
include_directories(${CMAKE_CURRENT_BINARY_DIR})
add_compile_definitions(
  VERSION_MAJOR=${tutuicmptunnel_VERSION_MAJOR}
  VERSION_MINOR=${tutuicmptunnel_VERSION_MINOR}
  VERSION_STR=\"${tutuicmptunnel_VERSION}\"
)

option(USE_SYSTEM_LIBBPF_BPFTOOL "Use system libbpf and bpftool" OFF)
message(STATUS "Using system libbpf&bpftool=${USE_SYSTEM_LIBBPF_BPFTOOL}")
# 默认使用libsodium而不是tucrypto
option(USE_TUCRYPTO "Enable builtin crypto instead of libsodium" OFF)
message(STATUS "Use tucrypto: ${USE_TUCRYPTO}")
option(USE_LLVM_FOR_KMOD "Enable llvm for kmodule building" OFF)
option(DISABLE_BPF_TIMER 0)
message(STATUS "Disable BPF timer=${DISABLE_BPF_TIMER}")
option(ENABLE_HARDEN_MODE "Enable security hardening compiler and linker flags" OFF)
option(ENABLE_XDP_INGRESS "Enable XDP ingress mode" OFF)
message(STATUS "Enable xdp ingress=${ENABLE_XDP_INGRESS}")
set(BPF_CPU_VERSION "v3" CACHE STRING "BPF CPU version (empty, v1, v2, v3, v4 or probe)")
message(STATUS "BPF CPU Version=${BPF_CPU_VERSION}")
if (ENABLE_HARDEN_MODE)
  message(STATUS "Secure mode enabled. Adding security hardening flags.")
  add_compile_definitions(_FORTIFY_SOURCE=2)
  set(HARDEN_C_FLAGS
    #-Werror
    -Werror=implicit
    -Werror=incompatible-pointer-types
    -Werror=int-conversion
    -fPIE
  )
  include(CheckCCompilerFlag)
  macro(check_and_add_c_flag FLAG)
    string(REGEX REPLACE "[^A-Za-z0-9_]" "_" VAR_SUFFIX ${FLAG})
    check_c_compiler_flag("${FLAG}" "C_HAS_${VAR_SUFFIX}")
    if(C_HAS_${VAR_SUFFIX})
      message(STATUS "  -> Compiler supports: ${FLAG}")
      list(APPEND HARDEN_C_FLAGS "${FLAG}")
    else()
      message(STATUS "  -> Compiler does NOT support: ${FLAG} (skipping)")
    endif()
  endmacro()

  check_and_add_c_flag("-fstack-clash-protection")
  check_and_add_c_flag("-fstack-protector-strong")
  check_and_add_c_flag("-Wtrampolines")
  check_and_add_c_flag("-Wbidi-chars=any")
  check_and_add_c_flag("-fno-delete-null-pointer-checks")
  check_and_add_c_flag("-fno-strict-overflow")
  check_and_add_c_flag("-fno-strict-aliasing")
  check_and_add_c_flag("-ftrivial-auto-var-init=zero")
  check_and_add_c_flag("-fstrict-flex-arrays=3")
  check_and_add_c_flag("-mbranch-protection=standard")
  check_and_add_c_flag("-fexceptions")
  check_and_add_c_flag("-fcf-protection=full")
  if (NOT C_HAS__fcf_protection_full)
    check_and_add_c_flag("-fcf-protection=branch")
  endif()

  if(UNIX AND NOT APPLE)
    set(HARDEN_LINKER_FLAGS
          -pie
          -Wl,-z,nodlopen
          -Wl,-z,noexecstack
          -Wl,-z,relro
          -Wl,-z,now
          -Wl,--as-needed
          -Wl,--no-copy-dt-needed-entries
          -Wl,-z,max-page-size=4096
          -Wl,--gc-sections
    )
  endif()
endif()

if (ENABLE_XDP_INGRESS)
  add_definitions(-DENABLE_XDP_INGRESS=1)
endif()

if (NOT USE_SYSTEM_LIBBPF_BPFTOOL)
  execute_process(
    COMMAND git submodule update --init --recursive
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    RESULT_VARIABLE GIT_SUBMODULE_RESULT
  )

  if(NOT GIT_SUBMODULE_RESULT EQUAL 0)
    message(WARNING "git submodule update failed. Please check your git submodules.")
  endif()
endif()

configure_file(
  "${PROJECT_SOURCE_DIR}/tutu_csum_fixup/dkms.conf.in"
  "${PROJECT_SOURCE_DIR}/tutu_csum_fixup/dkms.conf"
  @ONLY
)

configure_file(
  "${PROJECT_SOURCE_DIR}/tutu_csum_fixup/Makefile.in"
  "${PROJECT_SOURCE_DIR}/tutu_csum_fixup/Makefile"
  @ONLY
)

#set(CMAKE_VERBOSE_MAKEFILE ON)

if(USE_LLVM_FOR_KMOD)
  list(APPEND KMOD_MAKE_ARGS "LLVM=${LLVM}")
endif()

if (NOT WIN32)
  add_custom_target(
    build_tutu_csum_fixup
    COMMAND $(MAKE) -C ${PROJECT_SOURCE_DIR}/tutu_csum_fixup ${KMOD_MAKE_ARGS}
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/tutu_csum_fixup
    COMMENT "Building external kernel module tutu_csum_fixup"
  )

  if(DEFINED ENV{ARCH})
    set(BPF_ARCH_PARAM "ARCH=$ENV{ARCH}")
  endif()

  if (DISABLE_BPF_TIMER)
    list(APPEND BPF_ARCH_PARAM DISABLE_BPF_TIMER=1)
  endif()

  if (USE_SYSTEM_LIBBPF_BPFTOOL)
    set(BPFTOOL bpftool)
  else()
    set(BPFTOOL ${PROJECT_SOURCE_DIR}/bpftool/src/root/usr/local/sbin/bpftool)
  endif()

  include(CheckCXXSourceRuns)
  execute_process(
    COMMAND ${CMAKE_MAKE_PROGRAM} --no-silent -v
    RESULT_VARIABLE MAKE_NOSILENT_RESULT
    OUTPUT_QUIET ERROR_QUIET
  )

  if(MAKE_NOSILENT_RESULT EQUAL 0)
    set(MAKE_NOSILENT_ARG "--no-silent")
  else()
    set(MAKE_NOSILENT_ARG "")
  endif()

  add_custom_target(
    build_bpf
    COMMAND ENABLE_XDP_INGRESS=${ENABLE_XDP_INGRESS} BPF_CPU_VERSION=${BPF_CPU_VERSION} $(MAKE) ${MAKE_NOSILENT_ARG} -C ${PROJECT_SOURCE_DIR}/bpf ${BPF_ARCH_PARAM} BPFTOOL=${BPFTOOL} V=1
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/bpf
    COMMENT "Building bpf"
  )

  if (NOT USE_SYSTEM_LIBBPF_BPFTOOL)
    include(CheckTypeSize)
    check_type_size("void*" SIZEOF_VOID_P)
    if(SIZEOF_VOID_P AND SIZEOF_VOID_P EQUAL 8)
      set(_libdir "lib64")
    else()
      set(_libdir "lib")
    endif()
    set(LIBBPF_BUILD_DIR "${PROJECT_SOURCE_DIR}/libbpf/src")
    set(LIBBPF_INSTALL_DIR "${LIBBPF_BUILD_DIR}/root/usr/${_libdir}")
    set(LIBBPF_A "${LIBBPF_INSTALL_DIR}/libbpf.a")

    if(POLICY CMP0070)
      list(JOIN HARDEN_C_FLAGS " " HARDEN_C_FLAGS_STR)
    else()
      string(REPLACE ";" " " HARDEN_C_FLAGS_STR "${HARDEN_C_FLAGS}")
    endif()

    add_custom_command(
      OUTPUT ${LIBBPF_A}
      COMMAND BUILD_STATIC_ONLY=y OBJDIR=build DESTDIR=root CC=${CMAKE_C_COMPILER} "EXTRA_CFLAGS=-I${CMAKE_SYSROOT}/usr/include -Wno-error ${CMAKE_C_FLAGS_RELEASE} ${HARDEN_C_FLAGS_STR}" $(MAKE) install V=1
      WORKING_DIRECTORY ${LIBBPF_BUILD_DIR}
      COMMENT "Building and installing libbpf"
    )

    add_custom_target(
      build_libbpf
      DEPENDS ${LIBBPF_A}
    )
    set_property(TARGET build_libbpf APPEND PROPERTY ADDITIONAL_CLEAN_FILES "${LIBBPF_A}")

    set(BPFTOOL_BUILD_DIR "${PROJECT_SOURCE_DIR}/bpftool/src")
    set(BPFTOOL_INSTALL_DIR "${BPFTOOL_BUILD_DIR}/root/usr/local/sbin")
    set(BPFTOOL_BIN "${BPFTOOL_INSTALL_DIR}/bpftool")

    add_custom_command(
      OUTPUT ${BPFTOOL_BIN}
      COMMAND DESTDIR=root $(MAKE) "EXTRA_CFLAGS=-Wno-error"  install
      WORKING_DIRECTORY ${BPFTOOL_BUILD_DIR}
      COMMENT "Building and installing bpftool"
    )

    add_custom_target(
      build_bpftool
      DEPENDS ${BPFTOOL_BIN}
    )
    set_property(TARGET build_bpftool APPEND PROPERTY ADDITIONAL_CLEAN_FILES "${BPFTOOL_BIN}")

    add_dependencies(build_bpf build_libbpf)
    add_dependencies(build_libbpf build_bpftool)
  endif()

  add_custom_target(
    clean_bpf
    COMMAND $(MAKE) -C ${PROJECT_SOURCE_DIR}/bpf clean
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/bpf
    COMMENT "Cleaning bpf"
  )

  add_subdirectory(tuctl)
  add_dependencies(tuctl build_bpf)
endif()

add_subdirectory(common)

# 查找libsodium
find_path(SODIUM_INCLUDE_DIR sodium.h PATH_SUFFIXES sodium)
find_library(SODIUM_LIBRARY sodium)

include(FindPackageHandleStandardArgs)
find_package_handle_standard_args(
  SODIUM
  REQUIRED_VARS SODIUM_INCLUDE_DIR SODIUM_LIBRARY
  FAIL_MESSAGE "Could not find libsodium."
)

if (SODIUM_FOUND)
  if (USE_TUCRYPTO)
    message(WARNING "libsodium is found, but overridden by tucrypto (USE_TUCRYPTO=ON)")
    set(USE_TUCRYPTO 1)
    set(USE_SODIUM 0)
  else()
    message(STATUS "libsodium found and in use")
    set(USE_TUCRYPTO 0)
    set(USE_SODIUM 1)
  endif()
else()
  # 找不到 libsodium，切换为 tucrypto
  message(WARNING "libsodium not found; switching to tucrypto")
  set(USE_TUCRYPTO 1)
  set(USE_SODIUM 0)
endif()

if(USE_TUCRYPTO)
  include(CheckCSourceCompiles)

  # poly1305需要检查int128可用性
  # 检测 __int128 可用（GCC/Clang 通常支持，MSVC 不支持）
  set(CODE_HAS_INT128 "
  #include <stdint.h>
  int main(void) {
    __int128 a = 0;
    unsigned __int128 b = 0;
    (void)a; (void)b;
    return 0;
  }")
  check_c_source_compiles("${CODE_HAS_INT128}" HAVE___INT128)

  # 检测 MSVC 64-bit
  if (MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(HAVE_MSVC_64BIT 1)
  else()
    set(HAVE_MSVC_64BIT 0)
  endif()

  # 检测 GCC/Clang 版本与 64-bit 目标（如需保持与你原逻辑一致）
  set(HAVE_GCC_4_4_64BIT 0)
  if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    if (CMAKE_SIZEOF_VOID_P EQUAL 8)
      if (CMAKE_C_COMPILER_ID STREQUAL "GNU")
        if (CMAKE_C_COMPILER_VERSION VERSION_GREATER_EQUAL 4.4)
          set(HAVE_GCC_4_4_64BIT 1)
        endif()
      else()
        set(HAVE_GCC_4_4_64BIT 1)
      endif()
    endif()
  endif()

  if (HAVE___INT128 OR HAVE_MSVC_64BIT OR HAVE_GCC_4_4_64BIT)
    set(POLY1305_HAVE_FAST_64BIT 1)
  else()
    set(POLY1305_HAVE_FAST_64BIT 0)
  endif()

  include(CheckIncludeFile)
  check_include_file(sys/random.h HAVE_SYS_RANDOM_H)

  add_subdirectory(tucrypto)
endif()

# 生成配置头
configure_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/tucrypto_config.h.in
  ${CMAKE_CURRENT_BINARY_DIR}/tucrypto_config.h
  @ONLY
)

add_subdirectory(tuserver)

if (WIN32)
  add_subdirectory(win32)
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Release")
  set(CMAKE_INSTALL_DO_STRIP ON)
endif()

# vim: set sw=2 ts=2 expandtab:
