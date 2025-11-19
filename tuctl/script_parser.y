%{
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "log.h"

// 引用外部定义
extern int dispatch(int argc, char **argv);

extern int script_lex();
extern int script_lineno;
void script_error(const char *s);

int script_exit_code = 0;

/* --- 参数构建逻辑 --- */
#define MAX_ARGS 256
static char *cmd_argv[MAX_ARGS];
static int   cmd_argc = 0;

static void reset_args() {
	for (int i = 0; i < cmd_argc; i++) {
		if (cmd_argv[i]) free(cmd_argv[i]);
		cmd_argv[i] = NULL;
	}
	cmd_argc = 0;
}

static void append_arg(char *arg) {
  if (cmd_argc < MAX_ARGS - 1) {
    cmd_argv[cmd_argc++] = arg;
    cmd_argv[cmd_argc] = NULL;
  } else {
    log_error("Error: Too many arguments at line %d", script_lineno);
    free(arg);
  }
}

static void execute_cmd() {
  if (cmd_argc > 0) {
    // 执行命令
    int ret = dispatch(cmd_argc, cmd_argv);
    if (ret != 0) {
      log_error("Command failed (error %d) at line %d", ret, script_lineno);
      script_exit_code = ret;
    }
  }
  // 无论成功失败，执行后必须清空参数，为下一条命令做准备
  reset_args();
}

%}

%define api.prefix {script_}

%union {
  char *sval;
}

%token <sval> WORD STRING
%token SEMICOLON EOL

%%

script:
  /* empty */
  | script line
  ;

command:
  args   { if (cmd_argc > 0) execute_cmd(); }
  ;

statements:
    command
  | statements SEMICOLON command
  ;

line:
    EOL                        /* 纯空行 */
  | statements EOL             /* 格式：cmd1; cmd2 (最后没有分号) */
  | statements SEMICOLON EOL   /* 格式：cmd1; cmd2; (最后有分号) */
  ;

args:
  arg
  | args arg
  ;

arg:
  WORD    { append_arg($1); }
  | STRING  { append_arg($1); }
  ;

%%

void script_error(const char *s) {
  log_error("Parse error at line %d: %s", script_lineno, s);
  reset_args();
}

// vim: set sw=2 ts=2 expandtab:
