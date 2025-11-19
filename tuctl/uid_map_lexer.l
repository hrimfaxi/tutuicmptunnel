%{
#include <stdio.h>
#include <string.h>
#include "tuctl.h"
#include "uid_map_parser.h"

static char *safe_strdup(const char *s) {
  char *p = strdup(s);
  if (!p) {
    fprintf(stderr, "[Fatal Error] Out of memory in lexer.\n");
    exit(EXIT_FAILURE);
  }
  return p;
}
%}

%option prefix="uid_map_lexer_"
%option yylineno
%option noyywrap
%option nounput
%option noinput

%%

#.*       { /* 忽略注释：匹配 # 到行尾的内容 */ }
[ \t\r]+  { /* 忽略空白符 */ }

[0-9]+   {
  uid_map_lexer_lval.ival = atoi(yytext);
  return INTEGER;
}

[a-zA-Z0-9_.-]+ {
  uid_map_lexer_lval.sval = safe_strdup(yytext);
  return STRING;
}

\n { return EOL; }

.  { return yytext[0]; /* 返回未知字符以便报错 */ }

%%

// vim: set sw=2 ts=2 expandtab:
