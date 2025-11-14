#pragma once

#include <stdarg.h>

enum log_level {
  LOG_ERROR,
  LOG_WARN,
  LOG_INFO,
  LOG_DEBUG,
  LOG_TRACE,
};

extern const char *log_prefixes[][2];
extern int         log_verbosity;

#define _(text)       text
#define gettext(text) _(text)
#define N_(text)      text

#ifdef WIN32
#define RED    ""
#define YELLOW ""
#define GREEN  ""
#define BLUE   ""
#define GRAY   ""
#define BOLD   ""
#define RESET  ""
#else
#define RED    "\x1B[31m"
#define YELLOW "\x1B[33m"
#define GREEN  "\x1B[32m"
#define BLUE   "\x1B[34m"
#define GRAY   "\x1B[90m"
#define BOLD   "\x1B[1m"
#define RESET  "\x1B[0m"
#endif

void log_any(int level, const char *fmt, ...);

#define log_error(fmt, ...) log_any(LOG_ERROR, fmt, ##__VA_ARGS__)
#define log_warn(fmt, ...)  log_any(LOG_WARN, fmt, ##__VA_ARGS__)
#define log_info(fmt, ...)  log_any(LOG_INFO, fmt, ##__VA_ARGS__)
#define log_debug(fmt, ...) log_any(LOG_DEBUG, fmt, ##__VA_ARGS__)
#define log_trace(fmt, ...) log_any(LOG_TRACE, fmt, ##__VA_ARGS__)
