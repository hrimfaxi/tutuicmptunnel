set(SOURCES
  parser.c
  resolve.c
  read_script.c
)

add_library(common ${SOURCES})
target_compile_options(common PUBLIC ${HARDEN_C_FLAGS})
target_link_options(common PUBLIC ${HARDEN_LINKER_FLAGS})

# vim: set sw=2 expandtab :
