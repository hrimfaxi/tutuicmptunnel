%{
#include "log.h"
#include "script_parser.h" // 包含 yylval 和 token 定义
#include <string.h>
#include <stdio.h>

/* 若最后一行没有以 EOL/; 结束，则在 EOF 时补一个 EOL 让 parser 收尾 */
static int last_was_terminator = 1;

static char *safe_strdup(const char *s) {
  char *p = strdup(s);
  if (!p) {
    fprintf(stderr, "[Fatal Error] Out of memory in lexer.\n");
    exit(EXIT_FAILURE);
  }
  return p;
}

%}

%option prefix="script_"
%option yylineno
%option noyywrap
%option noinput
%option nounput


%%

  /* ============================================================
   1. Line Continuation (Bash style)
   匹配反斜杠，可选的空白，然后是换行。
   动作：什么都不做 (忽略)，从而将下一行连接到当前行。
   ============================================================ */
\\[ \t]*\n  { /* 忽略：这是续行符 */ }

  /* 2. 注释：# 开头直到行尾 (注意：不要吃掉换行符，否则 parser 无法识别命令结束) */
#[^\n]* { /* 忽略注释内容 */ }

  /* 3. 命令分隔符 */
";" { last_was_terminator = 1; return SEMICOLON; }
\n  { last_was_terminator = 1; return EOL; }

  /* 4. 带双引号的字符串 */
\"[^"\n]*\" {
  // 去掉首尾引号
  char *p = safe_strdup(yytext + 1);
  p[strlen(p)-1] = 0;
  script_lval.sval = p;
  last_was_terminator = 0;
  return STRING;
}

  /* 5. 单引号字符串 (Bash style: 强引用，不转义) */
\'[^'\n]*\' {
  char *p = safe_strdup(yytext + 1);
  p[strlen(p)-1] = 0;
  script_lval.sval = p;
  last_was_terminator = 0;
  return STRING;
}

  /* 6. 普通单词/参数 */
[^ \t\n;"]+ {
  script_lval.sval = safe_strdup(yytext);
  last_was_terminator = 0;
  return WORD;
}

  /* 7. 忽略空白 */
[ \t]+ { /* 忽略 */ }

  /* 8. EOF：若最后一行未以 EOL/; 结束，补一个 EOL */
<<EOF>> {
  if (!last_was_terminator) {
    last_was_terminator = 1;
    return EOL;
  }
  return 0; /* EOF */
}

  /* 9. 错误处理 */
. { log_error("Unexpected character: %c at line %d", *yytext, yylineno); }

%%

// vim: set sw=2 ts=2 expandtab:
