#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "read_script.h"
#include "try.h"
#include "tuparser.h"

int ftello_safe(FILE *stream, off_t *offset) {
  off_t pos = ftello(stream);

  if (pos < 0)
    return -1;

  *offset = pos;

  return 0;
}

int fread_safe(void *ptr, size_t size, size_t nmemb, FILE *stream, size_t *read) {
  int    err;
  size_t n = fread(ptr, size, nmemb, stream);

  if (ferror(stream)) {
    err = -errno;
    goto out;
  }

  if (read)
    *read = n;
  err = 0;

out:
  return err;
}

int read_script(const char *path, char **out, size_t *out_len) {
  FILE  *fp   = NULL;
  char  *buf  = NULL;
  int    err  = -1;
  size_t n    = 0;
  size_t size = 0;
  off_t  file_size;

  if (!strcmp(path, "-")) {
    // 动态读stdin
    size_t cap = 4096;
    for (buf = try2_p(malloc(cap + 1), "malloc failed");
         try2(fread_safe(buf + size, 1, cap - size, stdin, &n), "fread: %s", strret), n;) {
      size += n;
      if (size == cap) {
        char *tmp;

        cap *= 2;
        tmp = try2_p(realloc(buf, cap + 1), "realloc failed");
        buf = tmp;
      }
    }
    buf[size] = '\0';

    *out = buf;
    if (out_len)
      *out_len = size;
    err = 0;
    goto err_cleanup;
  }

  fp = try2_p(fopen(path, "rb"), "fopen: %s: %s", path, strret);
  try2(fseek(fp, 0, SEEK_END), "fseek: %s", strret);
  try2(ftello_safe(fp, &file_size), "ftello: %s", strret);
  rewind(fp);

  if (file_size < 0 || file_size > (off_t) (SIZE_MAX - 1)) {
    err = -EOVERFLOW;
    goto err_cleanup;
  }

  size = (size_t) file_size;
  buf  = try2_p(malloc(size + 1), "malloc failed");
  try2(fread_safe(buf, 1, size, fp, &n), "fread: %s", strret);
  if (n != size) {
    if (feof(fp))
      log_error("fread: unexpected EOF");
    else
      log_error("fread: short read but not EOF?");
  }
  buf[n] = '\0';

  *out = buf;
  if (out_len)
    *out_len = size;
  err = 0;

err_cleanup:
  if (err && buf)
    free(buf);
  if (fp)
    fclose(fp);
  if (err && out)
    *out = NULL;
  return err;
}

// vim: set sw=2 expandtab :
