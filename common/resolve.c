#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "log.h"
#include "network.h"
#include "resolve.h"
#include "try.h"

static const uint8_t ipv4_mapped_prefix[] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xFF, 0xFF};

int ipv4_to_in6addr(__be32 ipv4, struct in6_addr *ip6) {
  _Static_assert(sizeof(ipv4_mapped_prefix) == 12, "ipv4 mapped prefix");
  uint8_t *p = ip6->s6_addr;

  memcpy(p, ipv4_mapped_prefix, sizeof(ipv4_mapped_prefix)), p += sizeof(ipv4_mapped_prefix);
  memcpy(p, &ipv4, sizeof(ipv4)), p += sizeof(ipv4);
  return 0;
}

int inaddr6_is_mapped_ipv4(const struct in6_addr *ipv6) {
  return memcmp(ipv6->s6_addr, ipv4_mapped_prefix, sizeof(ipv4_mapped_prefix)) == 0;
}

int ipv6_ntop(char ipstr[INET6_ADDRSTRLEN], const struct in6_addr *ipv6) {
  const void *bytes = (typeof(bytes)) ipv6->s6_addr;
  if (inaddr6_is_mapped_ipv4(ipv6)) {
    return inet_ntop(AF_INET, bytes + sizeof(ipv4_mapped_prefix), ipstr, INET6_ADDRSTRLEN) ? 0 : -EINVAL;
  }

  return inet_ntop(AF_INET6, ipv6, ipstr, INET6_ADDRSTRLEN) ? 0 : -EINVAL;
}

int resolve_ip_addr(int family, const char *address, struct in6_addr *out_addr) {
  struct in6_addr addr6;
  struct addrinfo hints =
                    {
                      .ai_family = family,
                      .ai_flags  = AI_ADDRCONFIG,
                    },
                  *res = NULL;
  int err, found = 0;

  if ((family == AF_UNSPEC || family == AF_INET6) && inet_pton(AF_INET6, address, &addr6) == 1) {
    *out_addr = addr6;
    err       = 0;
    goto err_cleanup;
  }

  struct in_addr addr4;
  if ((family == AF_UNSPEC || family == AF_INET) && inet_pton(AF_INET, address, &addr4) == 1) {
    ipv4_to_in6addr(addr4.s_addr, out_addr);
    err = 0;
    goto err_cleanup;
  }

  try2(getaddrinfo(address, NULL, &hints, &res), _("invalid address: %s (%s)"), address, gai_strerror(_ret));

  for (struct addrinfo *p = res; p != NULL; p = p->ai_next) {
    if (p->ai_family == AF_INET6 && (family == AF_UNSPEC || family == AF_INET6)) {
      struct sockaddr_in6 *sin6 = (struct sockaddr_in6 *) p->ai_addr;
      *out_addr                 = sin6->sin6_addr;
      found                     = 1;
      break;
    } else if (p->ai_family == AF_INET && (family == AF_UNSPEC || family == AF_INET)) {
      struct sockaddr_in *sin = (struct sockaddr_in *) p->ai_addr;
      ipv4_to_in6addr(sin->sin_addr.s_addr, out_addr);
      found = 1;
      break;
    }
  }
  freeaddrinfo(res);

  if (!found) {
    log_error("cannot resolve address: %s", address);
    err = -ENOENT;
    goto err_cleanup;
  }

  err = 0;
err_cleanup:
  return err;
}

// vim: set sw=2 ts=2 expandtab :
