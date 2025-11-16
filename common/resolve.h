#pragma once

struct in6_addr;
int resolve_ip_addr(int family, const char *address, struct in6_addr *out_addr);

// vim: set sw=2 ts=2 expandtab:
