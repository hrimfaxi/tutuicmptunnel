%{
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "log.h"
#include "tuctl.h"


extern int uid_map_lexer_lex(void);
extern int uid_map_lexer_lineno;
extern FILE *uid_map_lexer_in;
void uid_map_lexer_error(uid_map_t *map, const char *s);

%}

%define api.prefix {uid_map_lexer_}

/* 定义联合体，用于存储 token 的值 */
%union {
    int ival;
    char *sval;
}

%parse-param { uid_map_t *map }

/* Token 定义 */
%token <ival> INTEGER
%token <sval> STRING
%token EOL

%destructor { free($$); } STRING

%%

config:
  /* empty */
  | config line
  ;

line:
    EOL
  | INTEGER STRING EOL {
      int uid = $1;
      char *name = $2;

      if (uid >= 0 && uid < UID_LEN) {
        if (map->hostnames[uid]) {
            log_warn("Duplicated UID %d at line %d, overwriting", uid, uid_map_lexer_lineno - 1);
            free(map->hostnames[uid]);
        }
        map->hostnames[uid] = name; /* 直接接管内存 */
      } else {
        log_warn("Invalid UID: %d out of range(0-%d) at line %d", uid, UID_LEN - 1, uid_map_lexer_lineno - 1);
        free(name); /* 必须释放，因为 strdup 分配了内存但未被使用 */
      }
  }
  | error EOL {
    /* 错误恢复：如果某行格式不对，跳过直到换行，继续解析下一行 */
    yyerrok;
  }
  ;

%%


void uid_map_lexer_error(uid_map_t *map, const char *s) {
  (void) map;
  log_warn("Parse error at line %d: %s", uid_map_lexer_lineno, s);
}

// vim: set sw=2 ts=2 expandtab:
