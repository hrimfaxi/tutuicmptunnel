#include <arpa/inet.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#include "common.h"

static inline uint64_t mono_ns(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint64_t) ts.tv_sec * 1000000000ull + (uint64_t) ts.tv_nsec;
}

// 简单 FNV-1a 64-bit
static uint64_t fnv1a64(const void *data, size_t len) {
  const uint8_t *p = (const uint8_t *) data;
  uint64_t       h = 1469598103934665603ull;
  for (size_t i = 0; i < len; i++) {
    h ^= p[i];
    h *= 1099511628211ull;
  }
  return h;
}

typedef struct {
  uint8_t  family;
  uint16_t port;
  union {
    struct in_addr  v4;
    struct in6_addr v6;
  } addr;
} rl_key_t;

static void key_from_sockaddr(const struct sockaddr_storage *sa, rl_key_t *out) {
  memset(out, 0, sizeof(*out));
  if (sa->ss_family == AF_INET) {
    const struct sockaddr_in *sin = (const struct sockaddr_in *) sa;
    out->family                   = AF_INET;
#ifdef RL_INCLUDE_PORT
    out->port = ntohs(sin->sin_port);
#endif
    out->addr.v4 = sin->sin_addr;
  } else if (sa->ss_family == AF_INET6) {
    const struct sockaddr_in6 *sin6 = (const struct sockaddr_in6 *) sa;
    out->family                     = AF_INET6;
#ifdef RL_INCLUDE_PORT
    out->port = ntohs(sin6->sin6_port);
#endif
    out->addr.v6 = sin6->sin6_addr;
  } else {
    // 未知族，置零即可，后续会合到一个桶里（也可直接拒绝）
    out->family = 0;
  }
}

static uint64_t hash_key(const rl_key_t *k) {
  return fnv1a64(k, sizeof(*k));
}

static int key_equal(const rl_key_t *k, const rl_entry_t *e) {
  if (!e->in_use || k->family != e->family)
    return 0;
#ifdef RL_INCLUDE_PORT
  if (k->port != e->port)
    return 0;
#endif
  if (k->family == AF_INET) {
    return memcmp(&k->addr.v4, &e->addr.v4, sizeof(struct in_addr)) == 0;
  } else if (k->family == AF_INET6) {
    return memcmp(&k->addr.v6, &e->addr.v6, sizeof(struct in6_addr)) == 0;
  }
  return 0;
}

void rl_init(rate_limiter_t *rl) {
  memset(rl, 0, sizeof(*rl));
}

// 返回 1=允许并消耗1令牌，0=拒绝（应丢包）
int rl_allow(rate_limiter_t *rl, const struct sockaddr_storage *sa) {
  rl_key_t key;
  key_from_sockaddr(sa, &key);
  uint64_t h   = hash_key(&key);
  uint64_t now = mono_ns();

  // 线性探测（限制步数，避免在 DoS 下长探测）
  const int max_probe = 16;
  int       free_idx  = -1;
  int       evict_idx = -1;

  for (int i = 0; i < max_probe; i++) {
    size_t      idx = (size_t) ((h + (uint64_t) i) & (RL_TABLE_SIZE - 1));
    rl_entry_t *e   = &rl->entries[idx];
    if (!e->in_use) {
      if (free_idx < 0)
        free_idx = (int) idx;
      break;
    }
    // 记录一个可淘汰的旧条目
    if ((now - e->last_seen_ns) > (uint64_t) RL_IDLE_EVICT_SEC * 1000000000ull) {
      evict_idx = (int) idx;
    }
    if (key_equal(&key, e)) {
      // 补充令牌
      double dt = (double) (now - e->last_refill_ns) / 1e9;
      if (dt > 0) {
        e->tokens = e->tokens + dt * RL_REFILL_RATE;
        if (e->tokens > RL_BURST_TOKENS)
          e->tokens = RL_BURST_TOKENS;
        e->last_refill_ns = now;
      }
      e->last_seen_ns = now;
      if (e->tokens >= 1.0) {
        e->tokens -= 1.0;
        return 1; // 允许
      } else {
        return 0; // 无令牌，丢包
      }
    }
  }

  // 没找到，尝试插入或复用
  int idx = free_idx >= 0 ? free_idx : evict_idx;
  if (idx >= 0) {
    rl_entry_t *e = &rl->entries[idx];
    memset(e, 0, sizeof(*e));
    e->in_use = 1;
    e->family = key.family;
#ifdef RL_INCLUDE_PORT
    e->port = key.port;
#endif
    if (key.family == AF_INET)
      e->addr.v4 = key.addr.v4;
    else if (key.family == AF_INET6)
      e->addr.v6 = key.addr.v6;
    e->tokens         = RL_BURST_TOKENS - 1.0; // 新建时直接消耗一个
    e->last_refill_ns = e->last_seen_ns = now;
    return 1;
  }

  // 表满且探测未命中：保守起见拒绝，避免被放大攻击拖慢
  return 0;
}

// vim: set sw=2 ts=2 expandtab:
