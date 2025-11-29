#pragma once

#include <stdbool.h>
#include <stdint.h>

int parse_u16(const char *input, uint16_t *out_u16);
int parse_sport(const char *input, uint16_t *out_sport);
int parse_port(const char *input, uint16_t *out_port);
int parse_icmp_id(const char *input, uint16_t *out_icmp_id);
int parse_uid(const char *input, uint8_t *out_uid);
int parse_u32(const char *input, uint32_t *out_u32);
int parse_age(const char *input, uint32_t *out_age);
int parse_window(const char *input, uint32_t *out_window);

void strip_inline_comment(char *s);

bool matches(const char *arg, const char *keyword);
bool is_address_kw(const char *arg);
bool is_help_kw(const char *arg);
bool is_user_kw(const char *arg);

int strdup_safe(const char *src, char **dst);

struct list_head;
void free_args_list(struct list_head *head);
int  split_args_list(const char *line, struct list_head *head);
int  args_list_to_argv(struct list_head *head, int *argc_out, char ***argv_out);

int escapestr(const char *str, char **escaped);

// vim: set sw=2 ts=2 expandtab:
