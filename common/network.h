#pragma once

#include <stdint.h>

#ifdef _WIN32
#include <winsock2.h>

#include <windows.h>
#include <ws2tcpip.h>

#if defined(_MSC_VER)
#include <intrin.h>
#define bswap16 _byteswap_ushort
#define bswap32 _byteswap_ulong
#define bswap64 _byteswap_uint64
#else
#define bswap16 _byteswap_ushort
#define bswap32 _byteswap_ulong
#define bswap64 _byteswap_uint64
#endif

static inline uint64_t htobe64(uint64_t x) {
  return bswap64(x);
}
static inline uint64_t be64toh(uint64_t x) {
  return bswap64(x);
}
typedef uint32_t __be32;
typedef uint16_t __be16;
#else
#include <arpa/inet.h>
#include <endian.h>
#include <linux/types.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

// vim: set sw=2 ts=2 expandtab:
