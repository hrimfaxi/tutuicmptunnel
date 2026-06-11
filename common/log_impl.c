#include "log.h"

#include <stdarg.h>
#include <stdio.h>

const char *log_prefixes[][2] = {
  {BOLD RED, N_("Error")},  {BOLD YELLOW, N_(" Warn")}, {BOLD GREEN, N_(" Info")},
  {BOLD BLUE, N_("Debug")}, {BOLD GRAY, N_("Trace")},
};

int log_verbosity = 2;

void log_any(int level, const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);

  // 防御非法level
  if (level < LOG_ERROR)
    level = LOG_ERROR;
  if (level > LOG_TRACE)
    level = LOG_TRACE;

  if (log_verbosity >= level) {
    fprintf(stderr, "%s%s " RESET, log_prefixes[level][0], gettext(log_prefixes[level][1]));
    if (level >= LOG_TRACE)
      fputs(GRAY, stderr);
    vfprintf(stderr, fmt, ap);
    if (level >= LOG_TRACE)
      fputs(RESET, stderr);
    fprintf(stderr, "\n");
  }
  va_end(ap);
}

// vim: set sw=2 ts=2 expandtab:
