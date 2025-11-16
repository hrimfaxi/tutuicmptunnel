#pragma once

#include <stdio.h>

int ftello_safe(FILE *stream, off_t *offset);
int fread_safe(void *ptr, size_t size, size_t nmemb, FILE *stream, size_t *read);
int read_script(const char *path, char **out, size_t *out_len);

// vim: set sw=2 ts=2 expandtab :
