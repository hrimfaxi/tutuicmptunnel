#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/time.h>
#include <time.h>

#include "../tucrypto/tucrypto.h"
#include "list.h"
#include "network.h"

#define _STR(x) #x
#define STR(x)  _STR(x)

#define SALT_LEN 16
#define TS_LEN   8

#define MIN_LEN     (SALT_LEN + TS_LEN + NONCE_LEN + TAG) // salt+ts+nonce+tag
#define MAX_CT_SIZE 1444
#define MAX_PT_SIZE (MAX_CT_SIZE - MIN_LEN)

#define DEFAULT_SERVER      "127.0.0.1"
#define DEFAULT_SERVER_PORT 14801
#define DEFAULT_WINDOW      30
#define DEFAULT_REPLAY_MAX  32768

// --- 重放窗口链表 ---
struct replay_entry {
  struct list_head list;
  time_t           ts;
  uint8_t          nonce[NONCE_LEN];
};

struct replay_window {
  int              window; // 秒
  int              max_size;
  int              count;
  struct list_head head;
};

struct sockaddr_storage;
int addr_to_str(const struct sockaddr_storage *addr, char *out, size_t len);
int psk2key(const char *psk, const uint8_t *salt, uint8_t *key);
int remove_padding(uint8_t *pt, unsigned long long *pt_len);
int encrypt_and_send_packet(int sock, const struct sockaddr *cli, socklen_t clen, struct replay_window *rwin, const char *psk,
                            const char *payload, size_t payload_len, size_t *out_packet_len);
int decrypt_and_validate_packet(uint8_t *pt_out, unsigned long long *pt_len_out, const uint8_t *pkt_in, ssize_t pkt_len,
                                struct replay_window *rwin, const char *psk, const struct sockaddr_storage *cli);

void setup_pwhash_memlimit(void);

// 重放相关
void replay_window_init(struct replay_window *rw, int window, int max_size);
void replay_window_free(struct replay_window *rw);
int  replay_check(struct replay_window *rw, time_t ts, const uint8_t nonce[NONCE_LEN]);
int  replay_add(struct replay_window *rw, time_t ts, const uint8_t nonce[NONCE_LEN]);

// 限制访问
#ifndef RL_TABLE_SIZE
#define RL_TABLE_SIZE 2048 // 槽位数（2^n 最好），嵌入式可调小些
#endif
#ifndef RL_BURST_TOKENS
#define RL_BURST_TOKENS 20.0 // 突发上限 B
#endif
#ifndef RL_REFILL_RATE
#define RL_REFILL_RATE 5.0 // 平均速率 r（tokens/秒），例如 5 pps
#endif
#ifndef RL_IDLE_EVICT_SEC
#define RL_IDLE_EVICT_SEC 60 // 空闲多久可被淘汰
#endif
// 若希望按“IP+端口”限速，置为 1
#undef RL_INCLUDE_PORT
// #define RL_INCLUDE_PORT 1

typedef struct {
  uint8_t  in_use;
  uint8_t  family; // AF_INET / AF_INET6
  uint16_t port;   // 可选：通常不用端口
  union {
    struct in_addr  v4;
    struct in6_addr v6;
  } addr;
  double   tokens;         // 当前可用令牌
  uint64_t last_refill_ns; // 上次补充时间
  uint64_t last_seen_ns;   // 最近访问时间
} rl_entry_t;

typedef struct {
  rl_entry_t entries[RL_TABLE_SIZE];
} rate_limiter_t;

void rl_init(rate_limiter_t *rl);
int  rl_allow(rate_limiter_t *rl, const struct sockaddr_storage *sa);

size_t scnprintf(char *buf, size_t size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

int sleep_ms(unsigned int ms);

// vim: set sw=2 ts=2 expandtab:
