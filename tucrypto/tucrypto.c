#include "tucrypto.h"
#include "argon2.h"
#include "poly1305-donna.h"
#include "xchacha20.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

void tucrypto_randombytes_buf(void *buf, const size_t nbytes) {
  get_random_bytes(buf, nbytes);
}

void tucrypto_memzero(void *const pnt, const size_t len) {
  explicit_bzero(pnt, len);
}

static inline int random_u32(uint32_t *out) {
  return get_random_bytes(out, sizeof(*out));
}

// 返回 [0, upper_bound) 的均匀随机数；upper_bound 必须 > 0
// 语义对齐 libsodium: 若 upper_bound==0 通常返回 0（未定义）。这里做保护。
uint32_t tucrypto_randombytes_uniform(uint32_t upper_bound) {
  if (upper_bound <= 1)
    return 0; // 0或1都只会返回0

  // 计算可接受阈值：最大 32bit 值中不产生偏差的上界
  // limit = floor((2^32 / upper_bound)) * upper_bound - 1
  const uint32_t threshold = (uint32_t) (-upper_bound) % upper_bound;
  // 解释：-upper_bound 在无符号溢出中等于 2^32 - upper_bound，
  // 再对 upper_bound 取模得到 (2^32 % upper_bound)，
  // 等价于 2^32 - ((2^32 / upper_bound) * upper_bound)。
  // 这是 libsodium/NaCl 常用技巧。

  while (1) {
    uint32_t r;
    if (random_u32(&r) < 0) {
      // 获取随机失败时退化为模运算（或返回错误；这里简单返回 0）
      return 0;
    }
    if (r >= threshold) {
      return r % upper_bound;
    }
    // 否则丢弃并重试
  }

  return 0;
}

__attribute__((weak)) void _tuserver_dummy_symbol_to_prevent_memcmp_lto(const unsigned char *b1, const unsigned char *b2,
                                                                        const size_t len);
__attribute__((weak)) void _tuserver_dummy_symbol_to_prevent_memcmp_lto(const unsigned char *b1, const unsigned char *b2,
                                                                        const size_t len) {
  (void) b1;
  (void) b2;
  (void) len;
}

int tucrypto_memcmp(const void *const b1_, const void *const b2_, size_t len) {
  const unsigned char   *b1 = (const unsigned char *) b1_;
  const unsigned char   *b2 = (const unsigned char *) b2_;
  size_t                 i;
  volatile unsigned char d = 0U;

  _tuserver_dummy_symbol_to_prevent_memcmp_lto(b1, b2, len);
  for (i = 0U; i < len; i++) {
    d |= b1[i] ^ b2[i];
  }
  return (1 & ((d - 1) >> 8)) - 1;
}

/* 小端编码 64-bit */
static void le64enc(uint8_t out[8], uint64_t x) {
  for (int i = 0; i < 8; i++) {
    out[i] = (uint8_t) (x & 0xff);
    x >>= 8;
  }
}

/* Poly1305 按 IETF AEAD 规则更新：块对齐到 16 字节 */
static void poly1305_update_padded16(poly1305_context *ctx, const uint8_t *data, size_t len) {
  if (len)
    poly1305_update(ctx, data, len);
  size_t rem = len & 0x0f;
  if (rem) {
    uint8_t zero[16] = {0};
    poly1305_update(ctx, zero, 16 - rem);
  }
}

static int tucrypto_crypto_aead_xchacha20poly1305_ietf_encrypt_detached(uint8_t *c, uint8_t tag[TAG], const uint8_t *m,
                                                                        size_t mlen, const uint8_t *ad, size_t adlen,
                                                                        const uint8_t npub[NONCE_LEN],
                                                                        const uint8_t key[KEYB]) {
  XChaCha_ctx      ctx;
  uint8_t          block0[64];
  uint8_t          otk[32];
  uint8_t          ctr[8] = {};
  poly1305_context pctx;
  uint8_t          lens[16];

  xchacha_keysetup(&ctx, key, (uint8_t *) npub);
  xchacha_set_counter(&ctx, ctr);
  xchacha_keystream_bytes(&ctx, block0, sizeof(block0));
  memcpy(otk, block0, sizeof(otk));

  /* 计数器设置为1 */
  ctr[0] = 1;
  xchacha_set_counter(&ctx, ctr);
  xchacha_encrypt_bytes(&ctx, m, c, (uint32_t) mlen);

  poly1305_init(&pctx, otk);
  poly1305_update_padded16(&pctx, ad, adlen);
  poly1305_update_padded16(&pctx, c, mlen);
  le64enc(lens + 0, (uint64_t) adlen);
  le64enc(lens + 8, (uint64_t) mlen);
  poly1305_update(&pctx, lens, sizeof(lens));
  poly1305_finish(&pctx, tag);

  tucrypto_memzero(&ctx, sizeof(ctx));
  tucrypto_memzero(ctr, sizeof(ctr));
  tucrypto_memzero(block0, sizeof(block0));
  tucrypto_memzero(otk, sizeof(otk));
  tucrypto_memzero(&pctx, sizeof(pctx));
  return 0;
}

static int tucrypto_crypto_aead_xchacha20poly1305_ietf_decrypt_detached(uint8_t *m, const uint8_t *c, size_t clen,
                                                                        const uint8_t tag[TAG], const uint8_t *ad, size_t adlen,
                                                                        const uint8_t npub[NONCE_LEN],
                                                                        const uint8_t key[KEYB]) {
  XChaCha_ctx      ctx;
  uint8_t          block0[64];
  uint8_t          otk[32];
  uint8_t          comp_tag[TAG];
  uint8_t          ctr[8] = {};
  uint8_t          lens[16];
  poly1305_context pctx;
  int              err = -1;

  xchacha_keysetup(&ctx, key, (uint8_t *) npub);
  xchacha_set_counter(&ctx, ctr);
  xchacha_keystream_bytes(&ctx, block0, sizeof(block0));
  memcpy(otk, block0, sizeof(otk));

  poly1305_init(&pctx, otk);
  poly1305_update_padded16(&pctx, ad, adlen);
  poly1305_update_padded16(&pctx, c, clen);
  le64enc(lens + 0, (uint64_t) adlen);
  le64enc(lens + 8, (uint64_t) clen);
  poly1305_update(&pctx, lens, sizeof(lens));
  poly1305_finish(&pctx, comp_tag);

  if (tucrypto_memcmp(comp_tag, tag, sizeof(comp_tag))) {
    /* tag mismatch */
    goto cleanup;
  }

  ctr[0] = 1;
  xchacha_set_counter(&ctx, ctr);
  xchacha_decrypt_bytes(&ctx, c, m, (uint32_t) clen);

  err = 0;

cleanup:
  /* 清理所有敏感数据 */
  tucrypto_memzero(&ctx, sizeof(ctx));
  tucrypto_memzero(&pctx, sizeof(pctx));
  tucrypto_memzero(ctr, sizeof(ctr));
  tucrypto_memzero(block0, sizeof(block0));
  tucrypto_memzero(otk, sizeof(otk));
  tucrypto_memzero(comp_tag, sizeof(comp_tag));
  tucrypto_memzero(lens, sizeof(lens));
  return err;
}

/* combined 版：ciphertext||tag 一起输出 */
int tucrypto_crypto_aead_xchacha20poly1305_ietf_encrypt(uint8_t *c, size_t *clen, const uint8_t *m, size_t mlen,
                                                        const uint8_t *ad, size_t adlen, const uint8_t npub[NONCE_LEN],
                                                        const uint8_t key[KEYB]) {
  if (clen)
    *clen = mlen + TAG;

  uint8_t *c_data = c;
  uint8_t *c_tag  = c + mlen;

  return tucrypto_crypto_aead_xchacha20poly1305_ietf_encrypt_detached(c_data, c_tag, m, mlen, ad, adlen, npub, key);
}

int tucrypto_crypto_aead_xchacha20poly1305_ietf_decrypt(uint8_t *m, size_t *mlen, const uint8_t *c, size_t clen,
                                                        const uint8_t *ad, size_t adlen, const uint8_t npub[NONCE_LEN],
                                                        const uint8_t key[KEYB]) {
  if (clen < TAG)
    return -1;

  size_t         ctext_len = clen - TAG;
  const uint8_t *c_data    = c;
  const uint8_t *c_tag     = c + ctext_len;

  if (tucrypto_crypto_aead_xchacha20poly1305_ietf_decrypt_detached(m, c_data, ctext_len, c_tag, ad, adlen, npub, key) != 0)
    return -1;

  if (mlen)
    *mlen = ctext_len;

  return 0;
}

// vim: set sw=2 ts=2 expandtab:
