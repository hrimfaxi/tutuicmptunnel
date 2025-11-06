if (USE_SODIUM)
  include_directories(${SODIUM_INCLUDE_DIR})
endif()

set(CLIENT_SOURCES
  tuctl_client.c
  common.c
  log.c
)
add_executable(tuctl_client ${CLIENT_SOURCES})

set(LIBS
  common
  pthread
)
if (USE_TUCRYPTO)
  list(APPEND LIBS tucrypto)
endif()

if (USE_SODIUM)
  list(APPEND LIBS ${SODIUM_LIBRARY})
endif()

target_link_libraries(tuctl_client ${LIBS})
install(TARGETS tuctl_client RUNTIME DESTINATION bin)

set(SERVER_SOURCES
  tuctl_server.c
  ratelimiter.c
  common.c
  log.c
)
add_executable(tuctl_server ${SERVER_SOURCES})
target_link_libraries(tuctl_server ${LIBS})
install(TARGETS tuctl_server RUNTIME DESTINATION bin)

# vim: set sw=2 ts=2 expandtab:
