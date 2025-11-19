#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "log.h"
#include "try.h"
#include "tuctl.h"
#include "tuparser.h"

void uid_map_init(uid_map_t *map) {
  if (!map)
    return;

  memset(map, 0, sizeof(*map));
}

void uid_map_free(uid_map_t *map) {
  if (!map)
    return;

  for (int i = 0; i < UID_LEN; i++) {
    free(map->hostnames[i]);
    map->hostnames[i] = NULL;
  }
}

int uid_map_load(uid_map_t *map, const char *filepath) {
  int    err = -EINVAL, line_num = 0;
  FILE  *file = NULL;
  char  *line = NULL;
  size_t len  = 0;

  if (!map || !filepath)
    goto err_cleanup;

  file = try2_p(fopen(filepath, "r"));

  while (getline(&line, &len, file) != -1) {
    line_num++;

    strip_inline_comment(line);
    if (line[0] == '\0')
      continue;

    int  uid;
    char hostname_buf[256];

    if (sscanf(line, "%d %255s", &uid, hostname_buf) == 2) {
      if (uid >= 0 && uid < UID_LEN) {
        if (map->hostnames[uid]) {
          log_warn("Duplicated UID %d at line %d", uid, line_num);
          free(map->hostnames[uid]);
        }

        try2(strdup_safe(hostname_buf, &map->hostnames[uid]), "strdup failed");
      } else {
        log_warn("Invalid UID: %d out of range(0-%d) at line %d, ignoring...", uid, UID_LEN - 1, line_num);
      }
    }
  }

  err = 0;
err_cleanup:
  if (file)
    fclose(file);
  if (line)
    free(line);
  return err;
}

int uid_map_get_host(const uid_map_t *map, int uid, const char **result_hostname) {
  if (!map || !result_hostname)
    return -EINVAL;

  if (uid >= 0 && uid < UID_LEN && map->hostnames[uid]) {
    *result_hostname = map->hostnames[uid];
    return 0;
  }

  return -ENOENT;
}

int uid_map_get_uid(const uid_map_t *map, const char *hostname, int *result_uid) {
  if (!map || !hostname || !result_uid)
    return -EINVAL;

  for (int i = 0; i < UID_LEN; ++i) {
    if (map->hostnames[i] && !strcmp(map->hostnames[i], hostname)) {
      *result_uid = i;
      return 0;
    }
  }

  return -ENOENT;
}

/* vim: set sw=2 expandtab : */
