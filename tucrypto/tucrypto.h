#pragma once

#include <stddef.h>
#include <stdint.h>

#include "tucrypto_config.h"

#ifdef USE_TUCRYPTO
#include "../tucrypto/argon2.h"

void     tucrypto_randombytes_buf(void *buf, const size_t nbytes);
void     tucrypto_memzero(void *ptr, size_t nbytes);
uint32_t tucrypto_randombytes_uniform(uint32_t upper_bound);
int      tucrypto_memcmp(const void *const b1_, const void *const b2_, size_t len);
#elif defined USE_SODIUM
#include "sodium.h"

#define tucrypto_randombytes_buf     randombytes_buf
#define tucrypto_memzero             sodium_memzero
#define tucrypto_randombytes_uniform randombytes_uniform
#define tucrypto_memcmp              sodium_memcmp
#else
#error Invalid configuration
#endif

enum {
  CRYPTO_AEAD_XCHACHA20POLY1305_IETF_KEYBYTES  = 32,
  CRYPTO_AEAD_XCHACHA20POLY1305_IETF_NPUBBYTES = 24,
  CRYPTO_AEAD_XCHACHA20POLY1305_IETF_ABYTES    = 16,
  CRYPTO_PWHASH_ARGON2ID_MEMLIMIT_INTERACTIVE  = 67108864U,
};

#define SALT_LEN  16
#define TS_LEN    8
#define KEYB      CRYPTO_AEAD_XCHACHA20POLY1305_IETF_KEYBYTES
#define NONCE_LEN CRYPTO_AEAD_XCHACHA20POLY1305_IETF_NPUBBYTES
#define TAG       CRYPTO_AEAD_XCHACHA20POLY1305_IETF_ABYTES

int get_random_bytes(void *buf, size_t nbytes);

int tucrypto_crypto_aead_xchacha20poly1305_ietf_encrypt(uint8_t *c, size_t *clen, const uint8_t *m, size_t mlen,
                                                        const uint8_t *ad, size_t adlen, const uint8_t npub[NONCE_LEN],
                                                        const uint8_t key[KEYB]);

int tucrypto_crypto_aead_xchacha20poly1305_ietf_decrypt(uint8_t *m, size_t *mlen, const uint8_t *c, size_t clen,
                                                        const uint8_t *ad, size_t adlen, const uint8_t npub[NONCE_LEN],
                                                        const uint8_t key[KEYB]);

// vim: set sw=2 ts=2 expandtab:
