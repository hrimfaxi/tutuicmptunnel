#include "../tucrypto/tucrypto.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>

#include "common.h"
#include "log.h"
#include "parser.h"
#include "try.h"

// 地址字符串化
int addr_to_str(const struct sockaddr_storage *addr, char *out, size_t len) {
  int  err;
  char hbuf[NI_MAXHOST], sbuf[NI_MAXSERV];

  err = try2(getnameinfo((struct sockaddr *) addr,
                         (addr->ss_family == AF_INET) ? sizeof(struct sockaddr_in) : sizeof(struct sockaddr_in6), hbuf,
                         sizeof(hbuf), sbuf, sizeof(sbuf), NI_NUMERICHOST | NI_NUMERICSERV),
             "getnameinfo: %s", gai_strerror(_ret));
  err = try2(scnprintf(out, len, "[%s]:%s", hbuf, sbuf));

err_cleanup:
  return err;
}

static size_t pwhash_memlimit = CRYPTO_PWHASH_ARGON2ID_MEMLIMIT_INTERACTIVE;

int psk2key(const char *psk, const uint8_t *salt, uint8_t *key) {
#ifdef USE_TUCRYPTO
  if (!psk || !salt || !key)
    return -1;

  // Argon2id 原始哈希输出
  return argon2id_hash_raw(2U, pwhash_memlimit / 1024U, 1, psk, strlen(psk), salt, SALT_LEN, key, KEYB);
#elif defined(USE_SODIUM)
  return crypto_pwhash(key, KEYB, psk, strlen(psk), salt, crypto_pwhash_OPSLIMIT_INTERACTIVE, pwhash_memlimit,
                       crypto_pwhash_ALG_ARGON2ID13);
#else
#error Invalid configuration
#endif
}

void setup_pwhash_memlimit(void) {
  uint32_t out = 0;
  char    *p   = getenv("TUTUICMPTUNNEL_PWHASH_MEMLIMIT");

  if (!p)
    return;

  if (parse_u32(p, &out)) {
    log_error("invalid pwhash memory limit value: %s", p);
    return;
  }

  pwhash_memlimit = (size_t) out;
  log_info("pwhash memory limit set to %zu", pwhash_memlimit);
}

int remove_padding(uint8_t *pt, unsigned long long *pt_len) {
  long long i, len = (long long) *pt_len;

  if (!len)
    return 0;

  i = len - 1;
  while (i >= 0 && pt[i] == '#') {
    i--;
  }

  // 没有在右侧找到任何'#'字符，此时不写'\0'也不更新*pt_len
  if (i == len - 1)
    return 0;

  pt[i + 1] = '\0';
  *pt_len   = i + 1;
  return 0;
}

void replay_window_init(struct replay_window *rw, int window, int max_size) {
  rw->window   = window;
  rw->max_size = max_size;
  rw->count    = 0;
  INIT_LIST_HEAD(&rw->head);
}

void replay_window_free(struct replay_window *rw) {
  struct list_head *p, *n;

  list_for_each_safe(p, n, &rw->head) {
    struct replay_entry *e = list_entry(p, struct replay_entry, list);

    list_del(&e->list);
    free(e);
  }

  rw->count = 0;
}

// 返回1=新包，0=重放/过期
int replay_check(struct replay_window *rw, time_t ts, const uint8_t nonce[NONCE_LEN]) {
  struct list_head *p, *n;
  time_t            now = time(NULL);

  if (llabs(now - ts) > rw->window)
    return 0;

  // 查重
  list_for_each_safe(p, n, &rw->head) {
    struct replay_entry *e = list_entry(p, struct replay_entry, list);

    // 过期清理
    if (e->ts + rw->window < now) {
      list_del(&e->list);
      free(e);
      rw->count--;
      continue;
    }
    // 查重
    if (e->ts == ts && !tucrypto_memcmp(e->nonce, nonce, NONCE_LEN)) {
      return 0;
    }
  }

  return 1;
}

// 加入
int replay_add(struct replay_window *rw, time_t ts, const uint8_t nonce[NONCE_LEN]) {
  struct replay_entry *e = malloc(sizeof(*e));
  if (!e) {
    return -ENOMEM;
  }

  e->ts = ts;
  memcpy(e->nonce, nonce, NONCE_LEN);
  list_add_tail(&e->list, &rw->head);
  rw->count++;

  // 控制容量
  while (rw->count > rw->max_size) {
    struct replay_entry *old = list_first_entry(&rw->head, struct replay_entry, list);
    list_del(&old->list);
    free(old);
    rw->count--;
  }

  return 0;
}

int encrypt_and_send_packet(int sock, const struct sockaddr *cli, socklen_t clen, struct replay_window *rwin, const char *psk,
                            const char *payload, size_t payload_len, size_t *out_packet_len) {
  int                err = 0;
  uint8_t            key[KEYB];
  uint8_t           *packet     = NULL;
  unsigned long long ct_len     = 0;
  size_t             packet_len = SALT_LEN + TS_LEN + NONCE_LEN + payload_len + TAG;
  packet                        = try2_p(malloc(packet_len), "malloc failed");
  time_t ts                     = time(NULL);

  uint8_t *salt  = packet;
  uint8_t *ts_b  = salt + SALT_LEN;
  uint8_t *nonce = ts_b + TS_LEN;
  uint8_t *ct    = nonce + NONCE_LEN;

  tucrypto_randombytes_buf(salt, SALT_LEN);
  uint64_t ts_resp = htobe64((uint64_t) ts);
  memcpy(ts_b, &ts_resp, TS_LEN);
  tucrypto_randombytes_buf(nonce, NONCE_LEN);

  try2(psk2key(psk, salt, key), "derive key failed: %d", _ret);
#ifdef USE_TUCRYPTO
  size_t ct_len_t = (size_t) ct_len;
  try2(tucrypto_crypto_aead_xchacha20poly1305_ietf_encrypt(ct, &ct_len_t, (const uint8_t *) payload, payload_len, salt,
                                                           SALT_LEN + TS_LEN, nonce, key),
       "encryption failed: %d", _ret);
  ct_len = (unsigned long long) ct_len_t;
#elif defined(USE_SODIUM)
  try2(crypto_aead_xchacha20poly1305_ietf_encrypt(ct, &ct_len, (const uint8_t *) payload, payload_len, salt, SALT_LEN + TS_LEN,
                                                  NULL, nonce, key),
       "encryption failed: %d", _ret);
#else
#error Invalid configuration
#endif
  try2(sendto(sock, packet, packet_len, 0, cli, clen), "sendto: %s", strret);

  err = replay_add(rwin, ts, nonce);
  if (err) {
    char abuf[128];
    try2(addr_to_str((struct sockaddr_storage *) cli, abuf, sizeof(abuf)));
    log_error("cannot add to replay list: %s", abuf);
    goto err_cleanup;
  }

  if (out_packet_len)
    *out_packet_len = packet_len;
  err = 0;
err_cleanup:
  free(packet);
  tucrypto_memzero(key, KEYB);
  return err;
}

/**
 * @brief Validates, checks for replay, and decrypts an incoming packet.
 * @param[out] pt_out      Buffer to store the decrypted plaintext.
 * @param[out] pt_len_out  Pointer to store the length of the plaintext.
 * @param[in]  cli         Client address for logging purposes.
 * @return 0 on success, non-zero on any validation/decryption failure.
 */
int decrypt_and_validate_packet(uint8_t *pt_out, unsigned long long *pt_len_out, const uint8_t *pkt_in, ssize_t pkt_len,
                                struct replay_window *rwin, const char *psk, const struct sockaddr_storage *cli) {
  _Static_assert(sizeof(time_t) >= 8, "time_t must be at least 64 bit");

  int     err = 0;
  char    abuf[128];
  uint8_t key[KEYB];

  if (pkt_len < (ssize_t) MIN_LEN) {
    try2(addr_to_str(cli, abuf, sizeof(abuf)));
    log_error("drop: short packet from %s", abuf);
    err = -EINVAL;
    goto err_cleanup;
  }

  const uint8_t *salt   = pkt_in;
  const uint8_t *ts_b   = salt + SALT_LEN;
  const uint8_t *nonce  = ts_b + TS_LEN;
  const uint8_t *ct     = nonce + NONCE_LEN;
  size_t         ct_len = pkt_len - (ct - pkt_in);

  time_t ts = 0;
  memcpy(&ts, ts_b, sizeof(ts));
  ts = be64toh(ts);

  if (!replay_check(rwin, ts, nonce)) {
    try2(addr_to_str(cli, abuf, sizeof(abuf)));
    log_error("drop: replay/window from %s", abuf);
    err = -EACCES;
    goto err_cleanup;
  }

  try2(psk2key(psk, salt, key), "derive key failed: %d", _ret);

#ifdef USE_TUCRYPTO
  size_t pt_len_t = 0;
  err = tucrypto_crypto_aead_xchacha20poly1305_ietf_decrypt(pt_out, &pt_len_t, ct, ct_len, salt, SALT_LEN + TS_LEN, nonce, key);
  *pt_len_out = (unsigned long long) pt_len_t;
#elif defined(USE_SODIUM)
  err = crypto_aead_xchacha20poly1305_ietf_decrypt(pt_out, pt_len_out, NULL, ct, ct_len, salt, SALT_LEN + TS_LEN, nonce, key);
#else
#error Invalid configuration
#endif
  if (err) {
    try2(addr_to_str(cli, abuf, sizeof(abuf)));
    log_error("drop: decrypt/auth fail from %s", abuf);
    err = -EBADMSG; // Bad message
    goto err_cleanup;
  }

  err = replay_add(rwin, ts, nonce);
  if (err) {
    try2(addr_to_str(cli, abuf, sizeof(abuf)));
    log_error("cannot add to replay list: %s", abuf);
    goto err_cleanup;
  }

err_cleanup:
  tucrypto_memzero(key, KEYB);
  return err;
}

// 返回实际写入的字节数，不包括结尾的\0字符.
// size>0时总是使用NUL结束字符串
// 如果size==0，不写入并返回0
// 写入最多size-1个字符，保证\0结尾
size_t scnprintf(char *buf, size_t size, const char *fmt, ...) {
  int     n;
  va_list ap;

  if (size == 0) {
    return 0;
  }

  va_start(ap, fmt);
  n = vsnprintf(buf, size, fmt, ap);
  va_end(ap);

  if (n < 0) {
    buf[0] = '\0';
    return 0;
  }

  if ((size_t) n >= size) {
    return size - 1;
  } else {
    return (size_t) n;
  }
}

// vim: set sw=2 ts=2 expandtab:
