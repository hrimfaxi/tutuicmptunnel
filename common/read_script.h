#pragma once

#include <stdint.h>
#include <stdio.h>

int64_t file_tell64(FILE *stream);
int     fread_safe(void *ptr, size_t size, size_t nmemb, FILE *stream, size_t *read);
int     read_script(const char *path, char **out, size_t *out_len);

// vim: set sw=2 ts=2 expandtab :
