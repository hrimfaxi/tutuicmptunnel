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

set(WINDIVERT_DLL_SRC "windivert.dll")
set(WINDIVERT_SYS_SRC "WinDivert64.sys")

set(WINDIVERT_DLL_DST "${CMAKE_CURRENT_BINARY_DIR}/windivert.dll")
set(WINDIVERT_SYS_DST "${CMAKE_CURRENT_BINARY_DIR}/WinDivert64.sys")

add_custom_command(
    TARGET icmptune
    POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${WINDIVERT_DLL_SRC}" "${WINDIVERT_DLL_DST}"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${WINDIVERT_SYS_SRC}" "${WINDIVERT_SYS_DST}"
    COMMENT "Copying WinDivert runtime files (DLL/SYS) to build/win32"
)

# vim: set sw=2 ts=2 expandtab:
