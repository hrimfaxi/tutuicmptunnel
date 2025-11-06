#include "tucrypto.h"

#include <stdio.h>
#ifdef HAVE_SYS_RANDOM_H
#include <sys/random.h>
#endif
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

int get_random_bytes(void *buf, size_t nbytes) {
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

  return nbytes;
}
