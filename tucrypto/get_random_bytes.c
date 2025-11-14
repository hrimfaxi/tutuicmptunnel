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

  if (status != STATUS_SUCCESS) {
    // 这里简单返回负值表示错误，你也可以映射到 errno 风格
    return -1;
  }

#else
  ssize_t result;

#ifdef HAVE_SYS_RANDOM_H
  result = getrandom(buf, nbytes, 0);
#else
  result = -1;
#endif

  if (result == -1) {
    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
      return fd;
    }

    result = read(fd, buf, nbytes);
    close(fd);

    if (result != (ssize_t) nbytes) {
      return -EINVAL;
    }
  } else if (result != (ssize_t) nbytes) {
    return -EINVAL;
  }

#endif
  return (int) nbytes;
}

// vim: set sw=2 ts=2 expandtab:
