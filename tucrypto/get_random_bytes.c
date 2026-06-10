#include "tucrypto.h"

#include <errno.h>
#include <stddef.h>
#include <stdio.h>

#ifdef _WIN32
#include <windows.h>

#include <bcrypt.h>
#include <ntstatus.h>
#else
#ifdef HAVE_SYS_RANDOM_H
#include <sys/random.h>
#endif
#include <fcntl.h>
#include <unistd.h>
#endif

int get_random_bytes(void *buf, size_t nbytes) {
#ifdef _WIN32
  /*
   * Windows：使用 BCryptGenRandom
   *
   * BCRYPT_USE_SYSTEM_PREFERRED_RNG 表示使用系统默认的强随机源，
   * 不需要显式打开/关闭算法句柄。
   */
  NTSTATUS status = BCryptGenRandom(NULL, (PUCHAR) buf, (ULONG) nbytes, BCRYPT_USE_SYSTEM_PREFERRED_RNG);
  if (status != STATUS_SUCCESS)
    return -EIO;
#else
  size_t off = 0;

  if (nbytes > INT_MAX)
    return -EINVAL;

#ifdef HAVE_SYS_RANDOM_H
  while (off < nbytes) {
    ssize_t n = getrandom((char *) buf + off, nbytes - off, 0);
    if (n > 0) {
      off += (size_t) n;
      continue;
    }
    if (n < 0 && errno == EINTR)
      continue;
    break;
  }
#endif

  if (off < nbytes) {
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0)
      return -errno;

    while (off < nbytes) {
      ssize_t n = read(fd, (char *) buf + off, nbytes - off);
      if (n > 0) {
        off += (size_t) n;
        continue;
      }
      if (n < 0 && errno == EINTR)
        continue;
      close(fd);
      return -EIO;
    }
    close(fd);
  }

#endif
  return (int) nbytes;
}

// vim: set sw=2 ts=2 expandtab:
