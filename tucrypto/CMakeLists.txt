set(TUCRYPTO_SRCS
  argon2.c
  blake2b.c
  core.c
  encoding.c
  genkat.c
  get_random_bytes.c
  opt.c
  poly1305-donna.c
  thread.c
  tucrypto.c
  xchacha20.c
)

add_library(tucrypto ${TUCRYPTO_SRCS})
target_compile_definitions(tucrypto PRIVATE ARGON2_NO_THREADS)
target_compile_options(tucrypto PUBLIC ${HARDEN_C_FLAGS})
target_link_options(tucrypto PUBLIC ${HARDEN_LINKER_FLAGS})
# vim: set sw=2 ts=2 expandtab:
