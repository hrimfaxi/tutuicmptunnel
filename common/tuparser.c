#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#include "list.h"
#include "log.h"
#include "try.h"
#include "tuparser.h"

int parse_u16(const char *input, uint16_t *out_u16) {
  char         *endptr = NULL;
  unsigned long value;

  errno = 0;
  value = strtoul(input, &endptr, 0);
  if (endptr == input || *endptr || errno == ERANGE || value > UINT16_MAX) {
    log_error("Invalid u16: %s", input);
    return -EINVAL;
  }

  *out_u16 = (typeof(*out_u16)) value;
  return 0;
}

// 源端口允许为0
int parse_sport(const char *input, uint16_t *out_sport) {
  return parse_u16(input, out_sport);
}

int parse_port(const char *input, uint16_t *out_port) {
  int err;

  err = parse_sport(input, out_port);
  if (!err && !*out_port) {
    log_error("Invalid port: %s", input);
    err = -EINVAL;
  }

  return err;
}

int parse_icmp_id(const char *input, uint16_t *out_icmp_id) {
  return parse_u16(input, out_icmp_id);
}

int parse_uid(const char *input, uint8_t *out_uid) {
  int      err;
  uint16_t tmp = 0;

  err = parse_u16(input, &tmp);
  if (err)
    return err;

  if (tmp > 255) {
    log_error("Invalid UID: %s", input);
    return -EINVAL;
  }

  *out_uid = (typeof(*out_uid)) tmp;
  return err;
}

int parse_u32(const char *input, uint32_t *out_u32) {
  char         *endptr = NULL;
  unsigned long ulong_val;

  errno     = 0; // 复位errno： strtoul函数本身不保证成功时设置errno=0
  ulong_val = strtoul(input, &endptr, 0);
  if (endptr == input || *endptr || errno == ERANGE || ulong_val > UINT32_MAX) {
    log_error("Invalid u32: %s", input);
    return -EINVAL;
  }

  *out_u32 = (typeof(*out_u32)) ulong_val;
  return 0;
}

int parse_age(const char *input, uint32_t *out_age) {
  int err = parse_u32(input, out_age);

  if (err)
    return err;

  if (*out_age == 0) {
    log_error("Invalid age: %s", input);
    return -EINVAL;
  }

  return 0;
}

int parse_window(const char *input, uint32_t *out_window) {
  int err = parse_u32(input, out_window);

  if (err)
    return err;

  if (*out_window == 0) {
    log_error("Invalid window: %s", input);
    return -EINVAL;
  }

  return 0;
}

int matches(const char *arg, const char *keyword) {
  return strcasecmp(arg, keyword);
}

/* 可选：允许“addr”当“address” */
bool is_address_kw(const char *arg) {
  return matches(arg, "address") == 0 || matches(arg, "addr") == 0;
}

bool is_help_kw(const char *arg) {
  return matches(arg, "-h") == 0 || matches(arg, "--help") == 0 || matches(arg, "help") == 0;
}

bool is_user_kw(const char *arg) {
  return matches(arg, "uid") == 0 || matches(arg, "user") == 0;
}

int strdup_safe(const char *src, char **dst) {
  int   err;
  char *dup = strdup(src);

  if (!dup) {
    err = -ENOMEM;
    goto out;
  }

  *dst = dup;
  err  = 0;
out:
  return err;
}

// vim: set sw=2 ts=2 expandtab:
