set(icmptune_SOURCES
  icmptune.c
  log.c
)
add_executable(icmptune ${icmptune_SOURCES})

set(LIBS
  windivert
  common
  ws2_32
)

target_include_directories(icmptune PUBLIC ../common)
target_link_directories(icmptune PUBLIC .)
target_link_libraries(icmptune ${LIBS})
install(TARGETS icmptune RUNTIME DESTINATION bin)

set(windivert_dll_src "${CMAKE_CURRENT_SOURCE_DIR}/windivert.dll")
set(windivert_sys_src "${CMAKE_CURRENT_SOURCE_DIR}/windivert64.sys")

set(windivert_dll_dst "${CMAKE_CURRENT_BINARY_DIR}/windivert.dll")
set(windivert_sys_dst "${CMAKE_CURRENT_BINARY_DIR}/windivert64.sys")

# 只有当 src != dst 时才添加复制命令，避免 in-source build 自己复制自己
if(NOT windivert_dll_src STREQUAL windivert_dll_dst OR NOT windivert_sys_src STREQUAL windivert_sys_dst)
  add_custom_command(
    TARGET icmptune
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${windivert_dll_src}" "${windivert_dll_dst}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${windivert_sys_src}" "${windivert_sys_dst}"
    COMMENT "copying WinDivert runtime files (dll/sys) to build/win32"
  )
endif()

# vim: set sw=2 ts=2 expandtab:
