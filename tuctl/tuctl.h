#pragma once

#include <netinet/in.h>
#include <unistd.h>

#include "tutuicmptunnel.debug.skel.h"
#include "tutuicmptunnel.skel.h"
#define _STR(x) #x
#define STR(x)  _STR(x)

#define NELEMS(n) (sizeof(n) / sizeof(n[0]))

#define PIN_DIR "/sys/fs/bpf/" STR(PROJECT_NAME) "/"

#define UID_LEN         256
#define UID_CONFIG_PATH "/etc/tutuicmptunnel/uids"

struct tutuicmptunnel;
int auto_pin(struct tutuicmptunnel *bpf);

struct tutuicmptunnel_debug;
int auto_pin_debug(struct tutuicmptunnel_debug *bpf);
int unauto_pin(void);

int ipv4_to_in6addr(__be32 ipv4, struct in6_addr *ip6);
int ipv6_ntop(char ipstr[INET6_ADDRSTRLEN], const struct in6_addr *ipv6);
int inaddr6_is_mapped_ipv4(const struct in6_addr *ipv6);

typedef struct uid_map_st {
  char *hostnames[UID_LEN];
} uid_map_t;

void uid_map_init(uid_map_t *map);
void uid_map_free(uid_map_t *map);
int  uid_map_load(uid_map_t *map, const char *filepath);
int  uid_map_get_host(const uid_map_t *map, int uid, const char **result_hostname);
int  uid_map_get_uid(const uid_map_t *map, const char *hostname, int *result_uid);
