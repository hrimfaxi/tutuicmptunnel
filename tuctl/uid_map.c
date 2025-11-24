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

extern FILE *uid_map_lexer_in;
extern int   uid_map_lexer_lineno;
extern int   uid_map_lexer_parse(uid_map_t *map);
extern void  uid_map_lexer_lex_destroy(void); /* 用于清理 Flex 内存 */

int uid_map_load(uid_map_t *map, const char *filepath) {
  FILE *file = NULL;
  int   err;

  if (!map || !filepath)
    return -EINVAL;

  file = try2_p(fopen(filepath, "r"));

  uid_map_lexer_lineno = 1;
  uid_map_lexer_in     = file;
  try2(uid_map_lexer_parse(map) == 0 ? 0 : -1, "Failed to parse UID map file: %s", filepath);
  err = 0;
err_cleanup:
  uid_map_lexer_lex_destroy();
  if (file) {
    fclose(file);
    file = NULL;
  }

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

// vim: set sw=2 ts=2 expandtab:
