cmake_minimum_required(VERSION 3.16)
project(tutuicmptunnel VERSION 1.2 LANGUAGES C)

if(NOT CMAKE_BUILD_TYPE)
  message(FATAL_ERROR "CMAKE_BUILD_TYPE is not set. Please set it to continue.\n"
                      "Example: cmake -DCMAKE_BUILD_TYPE=Release ..\n"
                      "Valid types: Release, Debug, RelWithDebInfo, MinSizeRel")
endif()

set(CMAKE_C_STANDARD 11)
add_compile_options(-Wall -Wextra -Wno-error)
add_definitions(-D_GNU_SOURCE=1 -DPROJECT_NAME=${CMAKE_PROJECT_NAME} -DINSTALL_PREFIX_DIR=${CMAKE_INSTALL_PREFIX})
include_directories(common)
add_compile_definitions(
  VERSION_MAJOR=${tutuicmptunnel_VERSION_MAJOR}
  VERSION_MINOR=${tutuicmptunnel_VERSION_MINOR}
  VERSION_STR=\"${tutuicmptunnel_VERSION}\"
)

option(USE_SYSTEM_LIBBPF_BPFTOOL "Use system libbpf and bpftool" OFF)
message(STATUS "Using system libbpf&bpftool=${USE_SYSTEM_LIBBPF_BPFTOOL}")
option(USE_LLVM_FOR_KMOD 1)
option(DISABLE_BPF_TIMER 0)
message(STATUS "Disable BPF timer=${DISABLE_BPF_TIMER}")
option(ENABLE_HARDEN_MODE "Enable security hardening compiler and linker flags" OFF)
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
  COMMAND BPF_CPU_VERSION=${BPF_CPU_VERSION} $(MAKE) ${MAKE_NOSILENT_ARG} -C ${PROJECT_SOURCE_DIR}/bpf ${BPF_ARCH_PARAM} BPFTOOL=${BPFTOOL} V=1
  WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/bpf
  COMMENT "Building bpf"
)

if (NOT USE_SYSTEM_LIBBPF_BPFTOOL)
  add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/libbpf_installed.stamp
    COMMAND BUILD_STATIC_ONLY=y OBJDIR=build DESTDIR=root CC=${CMAKE_C_COMPILER} "EXTRA_CFLAGS=-I${CMAKE_SYSROOT}/usr/include -Wno-error ${CMAKE_C_FLAGS_RELEASE}" $(MAKE) install V=1
    COMMAND ${CMAKE_COMMAND} -E touch ${CMAKE_CURRENT_BINARY_DIR}/libbpf_installed.stamp
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/libbpf/src
    COMMENT "Building and installing libbpf"
  )

  add_custom_target(
    build_libbpf
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/libbpf_installed.stamp
  )

  add_custom_command(
    OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/bpftool_installed.stamp
    COMMAND DESTDIR=root $(MAKE) "EXTRA_CFLAGS=-Wno-error"  install
    WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/bpftool/src
    COMMENT "Building and installing bpftool"
  )

  add_custom_target(
    build_bpftool
    DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/bpftool_installed.stamp
  )

  add_dependencies(build_bpf build_libbpf)
  add_dependencies(build_libbpf build_bpftool)
endif()

add_custom_target(
  clean_bpf
  COMMAND $(MAKE) -C ${PROJECT_SOURCE_DIR}/bpf clean
  WORKING_DIRECTORY ${PROJECT_SOURCE_DIR}/bpf
  COMMENT "Cleaning bpf"
)

add_subdirectory(common)
add_subdirectory(tuctl)
add_dependencies(tuctl build_bpf)

# 查找libsodium
find_path(SODIUM_INCLUDE_DIR sodium.h PATH_SUFFIXES sodium)
find_library(SODIUM_LIBRARY sodium)

# 有libsodium就编译tuserver
if(SODIUM_INCLUDE_DIR AND SODIUM_LIBRARY)
  message(STATUS "Found libsodium - building tuserver")
  add_subdirectory(tuserver)
else()
  message(STATUS "libsodium not found - skipping tuserver")
endif()

if(CMAKE_BUILD_TYPE STREQUAL "Release")
  set(CMAKE_INSTALL_DO_STRIP ON)
endif()

# vim: set sw=2 expandtab :
